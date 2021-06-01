#include "rocblas_test.hpp"
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <regex>
#ifdef WIN32
#include <windows.h>
#define strcasecmp(A, B) _stricmp(A, B)
#else
#include <pthread.h>
#include <unistd.h>
#endif

/*********************************************
 * thread pool functions
 *********************************************/
void thread_pool::worker_thread()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while(!m_done)
    {
        m_cond.wait(lock, [&] { return m_done || !m_work_queue.empty(); });
        if(m_done)
            break;
        auto task    = std::move(m_work_queue.front().first);
        auto promise = std::move(m_work_queue.front().second);
        m_work_queue.pop();
        lock.unlock();
        task();
        promise.set_value();
        lock.lock();
    }
}

thread_pool::thread_pool()
{
    auto thread_count = std::thread::hardware_concurrency();
    do
        m_threads.push_back(std::thread([&] { worker_thread(); }));
    while(thread_count-- > 1);
}

thread_pool::~thread_pool()
{
    m_done = true;
    m_cond.notify_all();
    for(auto& thread : m_threads)
        thread.join();
}

void thread_pool::submit(std::function<void()> func, std::promise<void> promise)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_work_queue.push(std::make_pair(std::move(func), std::move(promise)));
    }
    m_cond.notify_one();
}

/*********************************************
 * stream pool functions
 *********************************************/
void stream_pool::reset(size_t numDevices, size_t numStreams)
{
    for(auto& streamvec : m_streams)
        for(auto& stream : streamvec)
            CHECK_HIP_ERROR(hipStreamDestroy(stream));

    m_streams.clear();

    for(size_t i = 0; i < (numDevices > 1 ? numDevices : 1); ++i)
    {
        if(numDevices)
            CHECK_HIP_ERROR(hipSetDevice(i));
        std::vector<hipStream_t> temp;
        for(size_t j = 0; j < numStreams; ++j)
        {
            hipStream_t stream;
            CHECK_HIP_ERROR(hipStreamCreate(&stream));
            temp.push_back(stream);
        }
        m_streams.push_back(std::move(temp));
    }
}

/*********************************************
 * thread and stream pool variables
 *********************************************/
thread_pool g_thread_pool;
stream_pool g_stream_pool;

/*********************************************
 * callback function
 *********************************************/
thread_local std::unique_ptr<std::function<void(rocblas_handle)>> t_set_stream_callback;

/*********************************************
 * Signal-handling for detecting test faults *
 *********************************************/

// State of the signal handler
static thread_local struct
{
    // Whether sigjmp_buf is set and catching signals is enabled
    volatile sig_atomic_t enabled = false;

    // sigjmp_buf describing stack frame to go back to
#ifndef WIN32
    sigjmp_buf sigjmp_buf;
#else
    jmp_buf sigjmp_buf;
#endif

    // The signal which was received
    volatile sig_atomic_t signal;
} t_handler;

// Signal handler (must have external "C" linkage)
extern "C" void rocblas_test_signal_handler(int sig)
{
    int saved_errno = errno; // Save errno

    // If the signal handler is disabled, because we're not in the middle of
    // running a rocBLAS test, restore this signal's disposition to default,
    // and reraise the signal
    if(!t_handler.enabled)
    {
        signal(sig, SIG_DFL);
        errno = saved_errno;
        raise(sig);
        return;
    }

#ifndef WIN32
    // If this is an alarm timeout, we abort
    if(sig == SIGALRM)
    {
        static constexpr char msg[]
            = "\nAborting tests due to an alarm timeout.\n\n"
              "This could be due to a deadlock caused by mutexes being left locked\n"
              "after a previous test's signal was caught and partially recovered from.\n";
        // We must use write() because it's async-signal-safe and other IO might be blocked
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        rocblas_abort();
    }
#endif

    // Jump back to the handler code after setting handler.signal
    // Note: This bypasses stack unwinding, and may lead to memory leaks, but
    // it is better than crashing.
    t_handler.signal = sig;
    errno            = saved_errno;
#ifndef WIN32
    siglongjmp(t_handler.sigjmp_buf, true);
#else
    longjmp(t_handler.sigjmp_buf, true);
#endif
}

// Set up signal handlers
void rocblas_test_sigaction()
{
#ifndef WIN32
    struct sigaction act;
    act.sa_flags = 0;
    sigfillset(&act.sa_mask);
    act.sa_handler = rocblas_test_signal_handler;

    // Catch SIGALRM and synchronous signals
    for(int sig : {SIGALRM, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV})
        sigaction(sig, &act, nullptr);
#else
    for(int sig : {SIGABRT, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM})
        signal(sig, rocblas_test_signal_handler);
#endif
}

static const unsigned test_timeout = [] {
    // Number of seconds each test is allowed to take before all testing is killed.
    constexpr unsigned TEST_TIMEOUT = 600;
    unsigned           timeout;
    const char*        env = getenv("ROCBLAS_TEST_TIMEOUT");
    return env && sscanf(env, "%u", &timeout) == 1 ? timeout : TEST_TIMEOUT;
}();

// Lambda wrapper which detects signals and exceptions in an invokable function
void catch_signals_and_exceptions_as_failures(std::function<void()> test, bool set_alarm)
{
    // Save the current handler (to allow nested calls to this function)
    auto old_handler = t_handler;

#ifndef WIN32
    // Set up the return point, and handle siglongjmp returning back to here
    if(sigsetjmp(t_handler.sigjmp_buf, true))
    {
        FAIL() << "Received " << sys_siglist[t_handler.signal] << " signal";
    }
#else
    if(setjmp(t_handler.sigjmp_buf))
    {
        FAIL() << "Received signal";
    }
#endif
    else
    {
#ifndef WIN32
        // Alarm to detect deadlocks or hangs
        if(set_alarm)
            alarm(test_timeout);
#endif
        // Enable the signal handler
        t_handler.enabled = true;

        // Run the test function, catching signals and exceptions
        try
        {
            test();
        }
        catch(const std::exception& e)
        {
            FAIL() << "Received uncaught exception: " << e.what();
        }
        catch(...)
        {
            FAIL() << "Received uncaught exception";
        }
    }

#ifndef WIN32
    // Cancel the alarm if it was set
    if(set_alarm)
        alarm(0);
#endif
    // Restore the previous handler
    t_handler = old_handler;
}

void launch_test_on_streams(std::function<void()> test, size_t numStreams, size_t numDevices)
{
    size_t devices = numDevices > 1 ? numDevices : 1;
    size_t streams = numStreams > 1 ? numStreams : 1;
    for(size_t i = 0; i < devices; ++i)
    {
        hipSetDevice(i);
        for(size_t j = 0; j < streams; ++j)
        {
            if(numStreams)
                t_set_stream_callback.reset(
                    new std::function<void(rocblas_handle)>([=](rocblas_handle handle) {
                        rocblas_set_stream(handle, g_stream_pool[i][j]);
                    }));
            catch_signals_and_exceptions_as_failures(test, true);
        }
    }
}

void launch_test_on_threads(std::function<void()> test,
                            size_t                numThreads,
                            size_t                numStreams,
                            size_t                numDevices)
{
    auto promise = std::make_unique<std::promise<void>[]>(numThreads);
    auto future  = std::make_unique<std::future<void>[]>(numThreads);

    for(size_t i = 0; i < numThreads; ++i)
        future[i] = promise[i].get_future();

    for(size_t i = 0; i < numThreads; ++i)
        g_thread_pool.submit([=] { launch_test_on_streams(test, numStreams, numDevices); },
                             std::move(promise[i]));

    for(size_t i = 0; i < numThreads; ++i)
        future[i].get(); //Wait for tasks to complete
}

// Convert stream to normalized Google Test name
std::string RocBLAS_TestName_to_string(std::unordered_map<std::string, size_t>& table,
                                       std::ostringstream&                      str)
{
    std::string name{str.str()};

    // Remove trailing underscore
    if(!name.empty() && name.back() == '_')
        name.pop_back();

    // If name is empty, make it 1
    if(name.empty())
        name = "1";

    // Warn about unset letter parameters
    if(name.find('*') != name.npos)
        rocblas_cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
                        "Warning: Character * found in name."
                        " This means a required letter parameter\n"
                        "(e.g., transA, diag, etc.) has not been set in the YAML file."
                        " Check the YAML file.\n"
                        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                        "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                     << std::endl;

    // Replace non-alphanumeric characters with letters
    std::replace(name.begin(), name.end(), '-', 'n'); // minus
    std::replace(name.begin(), name.end(), '.', 'p'); // decimal point

    // Complex (A,B) is replaced with ArBi
    name.erase(std::remove(name.begin(), name.end(), '('), name.end());
    std::replace(name.begin(), name.end(), ',', 'r');
    std::replace(name.begin(), name.end(), ')', 'i');

    // If parameters are repeated, append an incrementing suffix
    auto p = table.find(name);
    if(p != table.end())
        name += "_t" + std::to_string(++p->second);
    else
        table[name] = 1;

    return name;
}

/********************************************************************************************
 * Function which matches Arguments with a category, accounting for arg.known_bug_platforms *
 ********************************************************************************************/
bool match_test_category(const Arguments& arg, const char* category)
{
    if(*arg.known_bug_platforms)
    {
        // Regular expression for token delimiters
        static const std::regex regex{"[:, \\f\\n\\r\\t\\v]+", std::regex_constants::optimize};

        // The name of the current GPU platform
        static const std::string platform = rocblas_internal_get_arch_name();

        // Token iterator
        std::cregex_token_iterator iter{arg.known_bug_platforms,
                                        arg.known_bug_platforms + strlen(arg.known_bug_platforms),
                                        regex,
                                        -1};

        // Iterate across tokens in known_bug_platforms, looking for matches with platform
        for(; iter != std::cregex_token_iterator(); ++iter)
        {
            // If a platform matches, set category to "known_bug"
            if(!strcasecmp(iter->str().c_str(), platform.c_str()))
            {
                // We know that underlying arg object is non-const, so we can use const_cast
                strcpy(const_cast<char*>(arg.category), "known_bug");
                break;
            }
        }
    }

    // Return whether arg.category matches the requested category
    return !strcmp(arg.category, category);
}
