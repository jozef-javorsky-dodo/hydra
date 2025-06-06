#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <queue>
#include <regex>
#include <semaphore>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include "db.hh"

#include <nix/store/derivations.hh>
#include <nix/store/derivation-options.hh>
#include <nix/store/pathlocks.hh>
#include <nix/util/pool.hh>
#include <nix/store/build-result.hh>
#include <nix/store/store-api.hh>
#include <nix/util/sync.hh>
#include "nar-extractor.hh"
#include <nix/store/serve-protocol.hh>
#include <nix/store/serve-protocol-impl.hh>
#include <nix/store/serve-protocol-connection.hh>
#include <nix/store/machines.hh>


typedef unsigned int BuildID;

typedef unsigned int JobsetID;

typedef std::chrono::time_point<std::chrono::system_clock> system_time;

typedef std::atomic<unsigned long> counter;


typedef enum {
    bsSuccess = 0,
    bsFailed = 1,
    bsDepFailed = 2, // builds only
    bsAborted = 3,
    bsCancelled = 4,
    bsFailedWithOutput = 6, // builds only
    bsTimedOut = 7,
    bsCachedFailure = 8, // steps only
    bsUnsupported = 9,
    bsLogLimitExceeded = 10,
    bsNarSizeLimitExceeded = 11,
    bsNotDeterministic = 12,
    bsBusy = 100, // not stored
} BuildStatus;


typedef enum {
    ssPreparing = 1,
    ssConnecting = 10,
    ssSendingInputs = 20,
    ssBuilding = 30,
    ssWaitingForLocalSlot = 35,
    ssReceivingOutputs = 40,
    ssPostProcessing = 50,
} StepState;


struct RemoteResult
{
    BuildStatus stepStatus = bsAborted;
    bool canRetry = false; // for bsAborted
    bool isCached = false; // for bsSucceed
    bool canCache = false; // for bsFailed
    std::string errorMsg; // for bsAborted

    unsigned int timesBuilt = 0;
    bool isNonDeterministic = false;

    time_t startTime = 0, stopTime = 0;
    unsigned int overhead = 0;
    nix::Path logFile;

    BuildStatus buildStatus() const
    {
        return stepStatus == bsCachedFailure ? bsFailed : stepStatus;
    }

    void updateWithBuildResult(const nix::BuildResult &);
};


struct Step;
struct BuildOutput;


class Jobset
{
public:

    typedef std::shared_ptr<Jobset> ptr;
    typedef std::weak_ptr<Jobset> wptr;

    static const time_t schedulingWindow = 24 * 60 * 60;

private:

    std::atomic<time_t> seconds{0};
    std::atomic<unsigned int> shares{1};

    /* The start time and duration of the most recent build steps. */
    nix::Sync<std::map<time_t, time_t>> steps;

public:

    double shareUsed()
    {
        return (double) seconds / shares;
    }

    void setShares(int shares_)
    {
        assert(shares_ > 0);
        shares = shares_;
    }

    time_t getSeconds() { return seconds; }

    void addStep(time_t startTime, time_t duration);

    void pruneSteps();
};


struct Build
{
    typedef std::shared_ptr<Build> ptr;
    typedef std::weak_ptr<Build> wptr;

    BuildID id;
    nix::StorePath drvPath;
    std::map<std::string, nix::StorePath> outputs;
    JobsetID jobsetId;
    std::string projectName, jobsetName, jobName;
    time_t timestamp;
    unsigned int maxSilentTime, buildTimeout;
    int localPriority, globalPriority;

    std::shared_ptr<Step> toplevel;

    Jobset::ptr jobset;

    std::atomic_bool finishedInDB{false};

    Build(nix::StorePath && drvPath) : drvPath(std::move(drvPath))
    { }

    std::string fullJobName()
    {
        return projectName + ":" + jobsetName + ":" + jobName;
    }

    void propagatePriorities();
};


struct Step
{
    typedef std::shared_ptr<Step> ptr;
    typedef std::weak_ptr<Step> wptr;

    nix::StorePath drvPath;
    std::unique_ptr<nix::Derivation> drv;
    std::unique_ptr<nix::DerivationOptions> drvOptions;
    nix::StringSet requiredSystemFeatures;
    bool preferLocalBuild;
    bool isDeterministic;
    std::string systemType; // concatenation of drv.platform and requiredSystemFeatures

    struct State
    {
        /* Whether the step has finished initialisation. */
        bool created = false;

        /* The build steps on which this step depends. */
        std::set<Step::ptr> deps;

        /* The build steps that depend on this step. */
        std::vector<Step::wptr> rdeps;

        /* Builds that have this step as the top-level derivation. */
        std::vector<Build::wptr> builds;

        /* Jobsets to which this step belongs. Used for determining
           scheduling priority. */
        std::set<Jobset::ptr> jobsets;

        /* Number of times we've tried this step. */
        unsigned int tries = 0;

        /* Point in time after which the step can be retried. */
        system_time after;

        /* The highest global priority of any build depending on this
           step. */
        int highestGlobalPriority{0};

        /* The highest local priority of any build depending on this
           step. */
        int highestLocalPriority{0};

        /* The lowest ID of any build depending on this step. */
        BuildID lowestBuildID{std::numeric_limits<BuildID>::max()};

        /* The time at which this step became runnable. */
        system_time runnableSince;

        /* The time that we last saw a machine that supports this
           step. */
        system_time lastSupported = std::chrono::system_clock::now();
    };

    std::atomic_bool finished{false}; // debugging

    nix::Sync<State> state;

    Step(const nix::StorePath & drvPath) : drvPath(drvPath)
    { }

    ~Step()
    {
        //printMsg(lvlError, format("destroying step %1%") % drvPath);
    }
};


void getDependents(Step::ptr step, std::set<Build::ptr> & builds, std::set<Step::ptr> & steps);

/* Call ‘visitor’ for a step and all its dependencies. */
void visitDependencies(std::function<void(Step::ptr)> visitor, Step::ptr step);


struct Machine : nix::Machine
{
    typedef std::shared_ptr<Machine> ptr;

    struct State {
        typedef std::shared_ptr<State> ptr;
        counter currentJobs{0};
        counter nrStepsDone{0};
        counter totalStepTime{0}; // total time for steps, including closure copying
        counter totalStepBuildTime{0}; // total build time for steps
        std::atomic<time_t> idleSince{0};

        struct ConnectInfo
        {
            system_time lastFailure, disabledUntil;
            unsigned int consecutiveFailures;
        };
        nix::Sync<ConnectInfo> connectInfo;

        /* Mutex to prevent multiple threads from sending data to the
           same machine (which would be inefficient). */
        std::timed_mutex sendLock;
    };

    State::ptr state;

    bool supportsStep(Step::ptr step)
    {
        /* Check that this machine is of the type required by the
           step. */
        if (!systemTypes.count(step->drv->platform == "builtin" ? nix::settings.thisSystem : step->drv->platform))
            return false;

        /* Check that the step requires all mandatory features of this
           machine. (Thus, a machine with the mandatory "benchmark"
           feature will *only* execute steps that require
           "benchmark".) The "preferLocalBuild" bit of a step is
           mapped to the "local" feature; thus machines that have
           "local" as a mandatory feature will only do
           preferLocalBuild steps. */
        for (auto & f : mandatoryFeatures)
            if (!step->requiredSystemFeatures.count(f)
                && !(f == "local" && step->preferLocalBuild))
                return false;

        /* Check that the machine supports all features required by
           the step. */
        for (auto & f : step->requiredSystemFeatures)
            if (!supportedFeatures.count(f)) return false;

        return true;
    }

    bool isLocalhost() const;

    // A connection to a machine
    struct Connection : nix::ServeProto::BasicClientConnection {
        // Backpointer to the machine
        ptr machine;
    };
};


class HydraConfig;


class State
{
private:

    std::unique_ptr<HydraConfig> config;

    // FIXME: Make configurable.
    const unsigned int maxTries = 5;
    const unsigned int retryInterval = 60; // seconds
    const float retryBackoff = 3.0;
    const unsigned int maxParallelCopyClosure = 4;

    /* Time in seconds before unsupported build steps are aborted. */
    const unsigned int maxUnsupportedTime = 0;

    nix::Path hydraData, logDir;

    bool useSubstitutes = false;

    /* The queued builds. */
    typedef std::map<BuildID, Build::ptr> Builds;
    nix::Sync<Builds> builds;

    /* The jobsets. */
    typedef std::map<std::pair<std::string, std::string>, Jobset::ptr> Jobsets;
    nix::Sync<Jobsets> jobsets;

    /* All active or pending build steps (i.e. dependencies of the
       queued builds). Note that these are weak pointers. Steps are
       kept alive by being reachable from Builds or by being in
       progress. */
    typedef std::map<nix::StorePath, Step::wptr> Steps;
    nix::Sync<Steps> steps;

    /* Build steps that have no unbuilt dependencies. */
    typedef std::list<Step::wptr> Runnable;
    nix::Sync<Runnable> runnable;

    /* CV for waking up the dispatcher. */
    nix::Sync<bool> dispatcherWakeup;
    std::condition_variable dispatcherWakeupCV;

    /* PostgreSQL connection pool. */
    nix::Pool<Connection> dbPool;

    /* The build machines. */
    std::mutex machinesReadyLock;
    typedef std::map<nix::StoreReference::Variant, Machine::ptr> Machines;
    nix::Sync<Machines> machines; // FIXME: use atomic_shared_ptr

    /* Throttler for CPU-bound local work. */
    static constexpr unsigned int maxSupportedLocalWorkers = 1024;
    std::counting_semaphore<maxSupportedLocalWorkers> localWorkThrottler;

    /* Various stats. */
    time_t startedAt;
    counter nrBuildsRead{0};
    counter buildReadTimeMs{0};
    counter nrBuildsDone{0};
    counter nrStepsStarted{0};
    counter nrStepsDone{0};
    counter nrStepsBuilding{0};
    counter nrStepsCopyingTo{0};
    counter nrStepsWaitingForDownloadSlot{0};
    counter nrStepsCopyingFrom{0};
    counter nrStepsWaiting{0};
    counter nrUnsupportedSteps{0};
    counter nrRetries{0};
    counter maxNrRetries{0};
    counter totalStepTime{0}; // total time for steps, including closure copying
    counter totalStepBuildTime{0}; // total build time for steps
    counter nrQueueWakeups{0};
    counter nrDispatcherWakeups{0};
    counter dispatchTimeMs{0};
    counter bytesSent{0};
    counter bytesReceived{0};
    counter nrActiveDbUpdates{0};

    /* Specific build to do for --build-one (testing only). */
    BuildID buildOne;
    bool buildOneDone = false;

    /* Statistics per machine type for the Hydra auto-scaler. */
    struct MachineType
    {
        unsigned int runnable{0}, running{0};
        system_time lastActive;
        std::chrono::seconds waitTime; // time runnable steps have been waiting
    };

    nix::Sync<std::map<std::string, MachineType>> machineTypes;

    struct MachineReservation
    {
        State & state;
        Step::ptr step;
        Machine::ptr machine;
        MachineReservation(State & state, Step::ptr step, Machine::ptr machine);
        ~MachineReservation();
    };

    struct ActiveStep
    {
        Step::ptr step;

        struct State
        {
            pid_t pid = -1;
            bool cancelled = false;
        };

        nix::Sync<State> state_;
    };

    nix::Sync<std::set<std::shared_ptr<ActiveStep>>> activeSteps_;

    std::atomic<time_t> lastDispatcherCheck{0};

    std::shared_ptr<nix::Store> localStore;
    std::shared_ptr<nix::Store> _destStore;

    size_t maxOutputSize;
    size_t maxLogSize;

    /* Steps that were busy while we encounted a PostgreSQL
       error. These need to be cleared at a later time to prevent them
       from showing up as busy until the queue runner is restarted. */
    nix::Sync<std::set<std::pair<BuildID, int>>> orphanedSteps;

    /* How often the build steps of a jobset should be repeated in
       order to detect non-determinism. */
    std::map<std::pair<std::string, std::string>, size_t> jobsetRepeats;

    bool uploadLogsToBinaryCache;

    /* Where to store GC roots. Defaults to
       /nix/var/nix/gcroots/per-user/$USER/hydra-roots, overridable
       via gc_roots_dir. */
    nix::Path rootsDir;

    std::string metricsAddr;

    struct PromMetrics
    {
        std::shared_ptr<prometheus::Registry> registry;

        prometheus::Counter& queue_checks_started;
        prometheus::Counter& queue_build_loads;
        prometheus::Counter& queue_steps_created;
        prometheus::Counter& queue_checks_early_exits;
        prometheus::Counter& queue_checks_finished;

        prometheus::Counter& dispatcher_time_spent_running;
        prometheus::Counter& dispatcher_time_spent_waiting;

        prometheus::Counter& queue_monitor_time_spent_running;
        prometheus::Counter& queue_monitor_time_spent_waiting;

        PromMetrics();
    };
    PromMetrics prom;

public:
    State(std::optional<std::string> metricsAddrOpt);

private:

    nix::MaintainCount<counter> startDbUpdate();

    /* Return a store object to store build results. */
    nix::ref<nix::Store> getDestStore();

    void clearBusy(Connection & conn, time_t stopTime);

    void parseMachines(const std::string & contents);

    /* Thread to reload /etc/nix/machines periodically. */
    void monitorMachinesFile();

    unsigned int allocBuildStep(pqxx::work & txn, BuildID buildId);

    unsigned int createBuildStep(pqxx::work & txn, time_t startTime, BuildID buildId, Step::ptr step,
        const std::string & machine, BuildStatus status, const std::string & errorMsg = "",
        BuildID propagatedFrom = 0);

    void updateBuildStep(pqxx::work & txn, BuildID buildId, unsigned int stepNr, StepState stepState);

    void finishBuildStep(pqxx::work & txn, const RemoteResult & result, BuildID buildId, unsigned int stepNr,
        const std::string & machine);

    int createSubstitutionStep(pqxx::work & txn, time_t startTime, time_t stopTime,
        Build::ptr build, const nix::StorePath & drvPath, const nix::Derivation drv, const std::string & outputName, const nix::StorePath & storePath);

    void updateBuild(pqxx::work & txn, Build::ptr build, BuildStatus status);

    void queueMonitor();

    void queueMonitorLoop(Connection & conn);

    /* Check the queue for new builds. */
    bool getQueuedBuilds(Connection & conn, nix::ref<nix::Store> destStore);

    /* Handle cancellation, deletion and priority bumps. */
    void processQueueChange(Connection & conn);

    BuildOutput getBuildOutputCached(Connection & conn, nix::ref<nix::Store> destStore,
        const nix::StorePath & drvPath);

    /* Returns paths missing from the remote store. Paths are processed in
     * parallel to work around the possible latency of remote stores. */
    std::map<nix::DrvOutput, std::optional<nix::StorePath>> getMissingRemotePaths(
        nix::ref<nix::Store> destStore,
        const std::map<nix::DrvOutput, std::optional<nix::StorePath>> & paths);

    Step::ptr createStep(nix::ref<nix::Store> store,
        Connection & conn, Build::ptr build, const nix::StorePath & drvPath,
        Build::ptr referringBuild, Step::ptr referringStep, std::set<nix::StorePath> & finishedDrvs,
        std::set<Step::ptr> & newSteps, std::set<Step::ptr> & newRunnable);

    void failStep(
        Connection & conn,
        Step::ptr step,
        BuildID buildId,
        const RemoteResult & result,
        Machine::ptr machine,
        bool & stepFinished);

    Jobset::ptr createJobset(pqxx::work & txn,
        const std::string & projectName, const std::string & jobsetName, const JobsetID);

    void processJobsetSharesChange(Connection & conn);

    void makeRunnable(Step::ptr step);

    /* The thread that selects and starts runnable builds. */
    void dispatcher();

    system_time doDispatch();

    void wakeDispatcher();

    void abortUnsupported();

    void builder(std::unique_ptr<MachineReservation> reservation);

    /* Perform the given build step. Return true if the step is to be
       retried. */
    enum StepResult { sDone, sRetry, sMaybeCancelled };
    StepResult doBuildStep(nix::ref<nix::Store> destStore,
        std::unique_ptr<MachineReservation> reservation,
        std::shared_ptr<ActiveStep> activeStep);

    void buildRemote(nix::ref<nix::Store> destStore,
        std::unique_ptr<MachineReservation> reservation,
        Machine::ptr machine, Step::ptr step,
        const nix::ServeProto::BuildOptions & buildOptions,
        RemoteResult & result, std::shared_ptr<ActiveStep> activeStep,
        std::function<void(StepState)> updateStep,
        NarMemberDatas & narMembers);

    void markSucceededBuild(pqxx::work & txn, Build::ptr build,
        const BuildOutput & res, bool isCachedBuild, time_t startTime, time_t stopTime);

    bool checkCachedFailure(Step::ptr step, Connection & conn);

    void notifyBuildStarted(pqxx::work & txn, BuildID buildId);

    void notifyBuildFinished(pqxx::work & txn, BuildID buildId,
        const std::vector<BuildID> & dependentIds);

    /* Acquire the global queue runner lock, or null if somebody else
       has it. */
    std::shared_ptr<nix::PathLocks> acquireGlobalLock();

    void dumpStatus(Connection & conn);

    void addRoot(const nix::StorePath & storePath);

    void runMetricsExporter();

public:

    void showStatus();

    void unlock();

    void run(BuildID buildOne = 0);
};
