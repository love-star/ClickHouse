#pragma once

#include <Processors/Formats/IOutputFormat.h>

#include <Common/ThreadPool.h>
#include <Common/Stopwatch.h>
#include <Common/logger_useful.h>
#include <Common/Exception.h>
#include <Common/CurrentMetrics.h>
#include <Common/CurrentThread.h>
#include <IO/WriteBufferFromString.h>
#include <Poco/Event.h>
#include <IO/BufferWithOwnMemory.h>
#include <IO/WriteBuffer.h>
#include <IO/NullWriteBuffer.h>

#include <deque>
#include <atomic>


namespace CurrentMetrics
{
    extern const Metric ParallelFormattingOutputFormatThreads;
    extern const Metric ParallelFormattingOutputFormatThreadsActive;
    extern const Metric ParallelFormattingOutputFormatThreadsScheduled;
}

namespace DB
{

class IOutputFormat;
using OutputFormatPtr = std::shared_ptr<IOutputFormat>;

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

/**
 * ORDER-PRESERVING parallel formatting of data formats.
 * The idea is similar to ParallelParsingInputFormat.
 * You add several Chunks through consume() method, each Chunk is formatted by some thread
 * in ThreadPool into a temporary buffer. (Formatting is being done in parallel.)
 * Then, another thread add temporary buffers into a "real" WriteBuffer.
 *
 *                   Formatters
 *      ↓   ↓   ↓   ↓   ↓   ↓   ↓   ↓   ↓   ↓
 *    ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
 *    | 1 | 2 | 3 | 4 | 5 | . | . | . | . | N | ← Processing units
 *    └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
 *      ↑               ↑
 *   Collector       addChunk
 *
 * There is a container of ProcessingUnits - internal entity, storing a Chunk to format,
 * a continuous memory buffer to store the formatted Chunk and some flags for synchronization needs.
 * Each ProcessingUnits has a unique number - the place in the container.
 * So, Chunk is added through addChunk method, which waits until current ProcessingUnit would be ready to insert
 * (ProcessingUnitStatus = READY_TO_INSERT), changes status to READY_TO_PARSE and spawns a new task in ThreadPool to parse it.
 * The other thread, we call it Collector waits until a ProcessingUnit which it points to would be READY_TO_READ.
 * Then it adds a temporary buffer to a real WriteBuffer.
 * Both Collector and a thread which adds Chunks have unit_number - a pointer to ProcessingUnit which they are aim to work with.
 *
 * Note, that collector_unit_number is always less or equal to current_unit_number, that's why the formatting is order-preserving.
 *
 * To stop the execution, a fake Chunk is added (ProcessingUnitType = FINALIZE) and finalize()
 * function is blocked until the Collector thread is done.
*/
class ParallelFormattingOutputFormat : public IOutputFormat
{
public:
    /// Used to recreate formatter on every new data piece.
    using InternalFormatterCreator = std::function<OutputFormatPtr(WriteBuffer & buf)>;

    /// Struct to simplify constructor.
    struct Params
    {
        WriteBuffer & out;
        SharedHeader header;
        InternalFormatterCreator internal_formatter_creator;
        const size_t max_threads_for_parallel_formatting;
    };

    ParallelFormattingOutputFormat() = delete;

    explicit ParallelFormattingOutputFormat(Params params)
        : IOutputFormat(params.header, params.out)
        , internal_formatter_creator(params.internal_formatter_creator)
        , pool(CurrentMetrics::ParallelFormattingOutputFormatThreads, CurrentMetrics::ParallelFormattingOutputFormatThreadsActive, CurrentMetrics::ParallelFormattingOutputFormatThreadsScheduled, params.max_threads_for_parallel_formatting)

    {
        LOG_TEST(getLogger("ParallelFormattingOutputFormat"), "Parallel formatting is being used");

        NullWriteBuffer buf;
        save_totals_and_extremes_in_statistics = internal_formatter_creator(buf)->areTotalsAndExtremesUsedInFinalize();
        buf.finalize();

        /// Just heuristic. We need one thread for collecting, one thread for receiving chunks
        /// and n threads for formatting.
        processing_units.resize(std::min(params.max_threads_for_parallel_formatting + 2, size_t{1024}));

        /// Do not put any code that could throw an exception under this line.
        /// Because otherwise the destructor of this class won't be called and this thread won't be joined.
        /// Also some race condition is possible, because collector_thread runs in parallel with
        /// the destruction of the objects already created in this scope.
        collector_thread = ThreadFromGlobalPool([thread_group = CurrentThread::getGroup(), this]
        {
            collectorThreadFunction(thread_group);
        });
    }

    ~ParallelFormattingOutputFormat() override
    {
        finishAndWait();
    }

    String getName() const override { return "ParallelFormattingOutputFormat"; }

    void flushImpl() override
    {
        if (!auto_flush)
            need_flush = true;
    }

    void writePrefix() override
    {
        addChunk(Chunk{}, ProcessingUnitType::START, /*can_throw_exception*/ true);
        started_prefix = true;
    }

    void onCancel() noexcept override
    {
        finishAndWait();
    }

    void writeSuffix() override
    {
        addChunk(Chunk{}, ProcessingUnitType::PLAIN_FINISH, /*can_throw_exception*/ true);
        started_suffix = true;
    }

    bool supportsWritingException() const override
    {
        NullWriteBuffer buffer;
        return internal_formatter_creator(buffer)->supportsWritingException();
    }

    void setException(const String & exception_message_) override { exception_message = exception_message_; }

private:
    void consume(Chunk chunk) final
    {
        addChunk(std::move(chunk), ProcessingUnitType::PLAIN, /*can_throw_exception*/ true);
    }

    void consumeTotals(Chunk totals) override
    {
        if (save_totals_and_extremes_in_statistics)
        {
            std::lock_guard lock(statistics_mutex);
            statistics.totals = std::move(totals);
        }
        else
        {
            addChunk(std::move(totals), ProcessingUnitType::TOTALS, /*can_throw_exception*/ true);
            are_totals_written = true;
        }
    }

    void consumeExtremes(Chunk extremes) override
    {
        if (save_totals_and_extremes_in_statistics)
        {
            std::lock_guard lock(statistics_mutex);
            statistics.extremes = std::move(extremes);
        }
        else
        {
            addChunk(std::move(extremes), ProcessingUnitType::EXTREMES, /*can_throw_exception*/ true);
        }
    }

    void finalizeImpl() override;

    void resetFormatterImpl() override
    {
        /// Resetting parallel formatting is not obvious and it's not used anywhere
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method resetFormatterImpl is not implemented for parallel formatting");
    }

    InternalFormatterCreator internal_formatter_creator;

    /// Status to synchronize multiple threads.
    enum ProcessingUnitStatus
    {
        READY_TO_INSERT,
        READY_TO_FORMAT,
        READY_TO_READ
    };

    /// Some information about what methods to call from internal parser.
    enum class ProcessingUnitType : uint8_t
    {
        START,
        PLAIN,
        PLAIN_FINISH,
        TOTALS,
        EXTREMES,
        FINALIZE,
    };

    void addChunk(Chunk chunk, ProcessingUnitType type, bool can_throw_exception);

    struct ProcessingUnit
    {
        std::atomic<ProcessingUnitStatus> status{ProcessingUnitStatus::READY_TO_INSERT};
        ProcessingUnitType type{ProcessingUnitType::START};
        Chunk chunk;
        Memory<> segment;
        size_t actual_memory_size{0};
        Statistics statistics;
        size_t rows_num;
    };

    Poco::Event collector_finished{};

    std::atomic_bool need_flush{false};

    // There are multiple "formatters", that's why we use thread pool.
    ThreadPool pool;
    // Collecting all memory to original ReadBuffer
    ThreadFromGlobalPool collector_thread;
    std::mutex collector_thread_mutex;

    std::exception_ptr background_exception = nullptr;

    /// We use deque, because ProcessingUnit doesn't have move or copy constructor.
    std::deque<ProcessingUnit> processing_units;

    std::mutex mutex;
    std::atomic_bool emergency_stop{false};

    std::atomic_size_t collector_unit_number{0};
    std::atomic_size_t writer_unit_number{0};

    std::condition_variable collector_condvar;
    std::condition_variable writer_condvar;

    size_t rows_consumed = 0;
    size_t rows_collected = 0;
    std::atomic_bool are_totals_written = false;

    /// We change statistics in onProgress() which can be called from different threads.
    std::mutex statistics_mutex;
    bool save_totals_and_extremes_in_statistics;

    String exception_message;
    bool exception_is_rethrown = false;
    bool started_prefix = false;
    bool collected_prefix = false;
    bool started_suffix = false;
    bool collected_suffix = false;
    bool collected_finalize = false;

    void finishAndWait() noexcept;

    void onBackgroundException()
    {
        std::lock_guard lock(mutex);
        if (!background_exception)
        {
            background_exception = std::current_exception();
        }
        emergency_stop = true;
        writer_condvar.notify_all();
        collector_condvar.notify_all();
    }

    void rethrowBackgroundException()
    {
        /// Rethrow background exception only once, because
        /// OutputFormat can be used after it to write an exception.
        if (!exception_is_rethrown)
        {
            exception_is_rethrown = true;
            std::rethrow_exception(background_exception);
        }
    }

    void scheduleFormatterThreadForUnitWithNumber(size_t ticket_number, size_t first_row_num)
    {
        pool.scheduleOrThrowOnError([this, thread_group = CurrentThread::getGroup(), ticket_number, first_row_num]
        {
            formatterThreadFunction(ticket_number, first_row_num, thread_group);
        });
    }

    /// Collects all temporary buffers into main WriteBuffer.
    void collectorThreadFunction(const ThreadGroupPtr & thread_group);

    /// This function is executed in ThreadPool and the only purpose of it is to format one Chunk into a continuous buffer in memory.
    void formatterThreadFunction(size_t current_unit_number, size_t first_row_num, const ThreadGroupPtr & thread_group);

    void setRowsBeforeLimit(size_t rows_before_limit) override
    {
        std::lock_guard lock(statistics_mutex);
        statistics.rows_before_limit = rows_before_limit;
        statistics.applied_limit = true;
    }
    void setRowsBeforeAggregation(size_t rows_before_aggregation) override
    {
        std::lock_guard lock(statistics_mutex);
        statistics.rows_before_aggregation = rows_before_aggregation;
        statistics.applied_aggregation = true;
    }
};

}
