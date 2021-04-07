#include <iostream>
#include "httplib.h"
#include "json.hpp"
#include <optional>
#include <vector>
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <set>
#include <queue>
#include <mutex>
#include <thread>
#include "blockingqueue.hpp"
#include <atomic>
#include <functional>

using nlohmann::json;

#define LOG(x)  std::cout << x << std::endl;
//#define LOG(x) ;

struct TimeStat
{
    double t = 0;
    int count = 0;
    std::map<int, int> hist;
};

std::ostream &operator << (std::ostream &s, const TimeStat &st) {
    s << st.t << " " << st.count << " " << (st.t / (st.count + 1e-6)) << " H ";
    for (auto &p : st.hist) {
        s << "| " << p.first << ": " << p.second;
    }
    return s;
}

std::map<std::string, TimeStat> _timeStats;
std::mutex timeStatsMutex;

TimeStat & getTimeStat(const std::string &key)
{
    std::lock_guard<std::mutex> g(timeStatsMutex);
    return _timeStats[key];
}

std::map<std::string, TimeStat> _cpuTimeStats;
std::mutex cpuTimeStatsMutex;

TimeStat & getCpuTimeStat(const std::string &key)
{
    std::lock_guard<std::mutex> g(cpuTimeStatsMutex);
    return _cpuTimeStats[key];
}

int roundT(double ms)
{
    if (ms < 3)
        return (int)(ms * 5) * 2;

    if (ms < 10)
        return (int)(ms) * 10;

    if (ms < 100)
        return (int)(ms / 10) * 100;

    return (int)(ms / 25) * 250;
}

struct ReqT {
    std::chrono::steady_clock::time_point s, e;
    std::string key;
};
std::vector<ReqT> allReq;

struct TimeMeasure
{
    std::string key_;

    TimeMeasure(const std::string &key) : total(getTimeStat(key)), key_(key) {
        c_start = std::chrono::steady_clock::now();
    }

    ~TimeMeasure()
    {
        std::chrono::steady_clock::time_point c_end = std::chrono::steady_clock::now();
        double dt = std::chrono::duration_cast<std::chrono::nanoseconds>(c_end - c_start).count() * 0.000001;

        std::lock_guard<std::mutex> g(timeStatsMutex);
        total.t += dt;
        total.count++;
        total.hist[roundT(dt)]++;

        ReqT reqT;
        reqT.s = c_start;
        reqT.e = c_end;
        reqT.key = key_;
        allReq.push_back(reqT);
    }

    TimeStat &total;
    std::chrono::steady_clock::time_point c_start;
};

struct CpuTimeMeasure
{
    CpuTimeMeasure(const std::string &key) : total(getCpuTimeStat(key)) {
        c_start = std::clock();
    }

    ~CpuTimeMeasure()
    {
        std::clock_t c_end = std::clock();
        double dt = 1000.0 * (c_end - c_start) / CLOCKS_PER_SEC;

        std::lock_guard<std::mutex> g(cpuTimeStatsMutex);
        total.t += dt;
        total.count++;
        total.hist[(int)(dt * 10)]++;
    }

    TimeStat &total;
    std::clock_t c_start;
};

struct P {
    int x, y;
};

struct Area {
    P pos, size;
};

struct License {
    int id;
    int digAllowed;
    int digUsed;
};

int license200 = 0;
int errLicense502 = 0; // {"code":502,"message":"RPC failed"}
int errLicense504 = 0; // {"code":504,"message":"RPC timed out"}
int license200WithCoin = 0;
int errLicense502WithCoin = 0; // {"code":502,"message":"RPC failed"}
int errLicense504WithCoin = 0; // {"code":504,"message":"RPC timed out"}
int errCash503 = 0;
int cash200 = 0;

struct Cost {
    std::atomic<int> c = 0;
    std::atomic<int> n = 0;
};

std::map<int, Cost> thCost;

void updateThCost(int depth, int coinsN)
{
    Cost &c = thCost[depth];
    c.c += coinsN;
    c.n++;

    if (coinsN == 0)
        LOG("ZERO COINS");
}

std::atomic<int> totalReqs = 0;
std::atomic<int> totalReqsT = 0;
std::atomic<bool> isRunning = true;
std::atomic<int> expCost = 0;

void closeAll();


/*
 Costs
Total 1.2kk points
exp 1*1 = 1 point
cash = 20 points
dig = 1 + (depth-1)/11.25
 * */

double getDigCost(int depth)
{
    return 2.0 * (1.0 + (depth - 1.0) / 11.25);
}

httplib::Result post(httplib::Client &cli, const char *addr, const std::string &body)
{
    ++totalReqs;

#ifdef MAX_REQS_T
    if (strcmp(addr, "/cash") == 0)
    {
        totalReqsT += 10;
    }
    else if (strcmp(addr, "/dig") == 0)
    {
        totalReqsT += 3;
    }
    if (strcmp(addr, "/explore") == 0)
    {
        totalReqsT += 3;
    }
    if (strcmp(addr, "/licenses") == 0)
    {
        totalReqsT += 58;
    }
    if (totalReqsT > MAX_REQS_T) {
        isRunning = false;
        closeAll();
        throw std::runtime_error("MAX_REQS_REACHED");
    }
#endif

    TimeMeasure postT(addr);

    httplib::Result res = cli.Post(
            addr, body.data(), body.size(),
            "application/json");

    if (!res) {
        isRunning = false;
        closeAll();
        throw std::runtime_error(std::string(addr) + " " + std::to_string(res.error()));
    }

    return res;
}

double exploreCost(int area)
{
    if (area <= 3)
        return 1;

    double res = 1;
    for (int i = 4; i <= area; i *= 2)
    {
        res += 0.75;
    }

    return res;
}

double exploreCostI(int area)
{
    if (area <= 3)
        return 4;

    int res = 1;
    for (int i = 4; i <= area; i *= 2)
    {
        res += 3;
    }

    return res;
}

double exploreCost(P area)
{
    return exploreCost(area.x * area.y);
}

std::optional<int> postExplore(httplib::Client &cli, const Area &area)
{
    expCost += exploreCostI(area.size.x * area.size.y);

    TimeMeasure postT("exp_" + std::to_string(area.size.x) + "_" + std::to_string(area.size.y));

    json req = {
            {"posX", area.pos.x},
            {"posY", area.pos.y},
            {"sizeX", area.size.x},
            {"sizeY", area.size.y}
    };

    auto reqStr = req.dump();
    auto res = post(cli, "/explore", reqStr);

    if (res->status != 200) {
        LOG("ERR explore " << reqStr << " " << res->status << " " << res->body);
        return std::nullopt;
    }

    auto resp = json::parse(res->body);
    int result = resp["amount"];

    /*if (result > 1) {
        LOG("SUC explore " << reqStr << " " << result);
    }*/

    return result;
}

std::mutex licenseDigsPerCoinsMutex;
std::map<int, std::set<int>> licenseDigsPerCoins;

std::optional<License> postLicense(httplib::Client &cli, std::vector<int> coins)
{
    json req = coins;
    auto reqStr = req.dump();

    auto res = post(cli, "/licenses", reqStr);

    if (res->status != 200) {
        if (!coins.empty())
        {
            if (res->status == 502)
                errLicense502WithCoin++;
            else if (res->status == 504)
                errLicense504WithCoin++;
            else
                LOG("ERR licenses " << reqStr << " " << res->status << " " << res->body);
        } else {
            if (res->status == 502)
                errLicense502++;
            else if (res->status == 504)
                errLicense504++;
            else
                LOG("ERR licenses " << reqStr << " " << res->status << " " << res->body);
        }
        return std::nullopt;
    }

    if (!coins.empty()) {
        license200WithCoin++;
    } else
    {
        license200++;
    }
    //LOG("SUC license " << res->body);
    auto resp = json::parse(res->body);
    License license {
        .id = resp["id"],
        .digAllowed = resp["digAllowed"],
        .digUsed = resp["digUsed"]
    };

    {
        std::lock_guard<std::mutex> g(licenseDigsPerCoinsMutex);
        licenseDigsPerCoins[coins.size()].insert(license.digAllowed - license.digUsed);
    }
    return license;
}

std::atomic_int digErrors = 0;
std::optional<std::vector<std::string>> postDig(httplib::Client &cli, int licenseId, P p, int depth)
{
    TimeMeasure postT("dig_" + std::to_string(depth));

    json req = {
            {"licenseID", licenseId},
            {"posX", p.x},
            {"posY", p.y},
            {"depth", depth}
    };

    auto reqStr = req.dump();
    auto res = post(cli, "/dig", reqStr);

    if (res->status == 404) {
        return std::vector<std::string>();
    }

    if (res->status != 200) {
        //LOG("ERR dig " << reqStr << " " << res->status << " " << res->body);
        digErrors++;
        return std::nullopt;
    }

    auto resp = json::parse(res->body);
    std::vector<std::string> result = resp;

    //if (!result.empty())
    //    LOG("SUC dig " << p.x << " " << p.y << " " << depth);

    return result;
}

std::optional<std::vector<int>> postCash(httplib::Client &cli, const std::string &treasure)
{
    json req = treasure;
    auto reqStr = req.dump();
    auto res = post(cli, "/cash", reqStr);

    if (res->status != 200) {
        if (res->status == 503)
            errCash503++;
        else
            LOG("ERR cash " << reqStr << " " << res->status << " " << res->body);
        return std::nullopt;
    }

    cash200++;
    auto resp = json::parse(res->body);
    std::vector<int> result = resp;

    //LOG("SUC cash " << treasure << " " << result.size());

    return result;
}

int log2(int v)
{
    int res = 0;
    while (v > 1) {
        v = (v + 1) / 2;
        ++res;
    }

    return res;
}

int simulateNumberOfExplores(int *coords, int size, int minC, int maxC)
{
    int d = maxC - minC;
    if (d <= 1 || size == 0)
        return 0;

    if (size == 1)
        return log2(d);

    int middle = minC + d/2;

    int c1 = 0;
    for (; c1 < size; )
    {
        if (coords[c1] >= middle)
            break;

        ++c1;
    }

    return simulateNumberOfExplores(coords, c1, minC, middle) + simulateNumberOfExplores(coords + c1, size - c1, middle, maxC) + 1;
}



double simulateNumberOfExplores(int d, int k)
{
    if (k == 1)
        return log2(d);

    if (d == 1)
        return 0;

    if (d == 2)
        return 1;

    std::vector<int> coords;
    coords.resize(k);

    double sum = 0;
    const int N = 100;
    for (int sim = 0; sim < N; ++sim)
    {
        for (int i = 0; i < k; ++i)
            coords[i] = rand() % d;

        std::sort(coords.begin(), coords.end());

        sum += simulateNumberOfExplores(coords.data(), k, 0, d);
    }

    return sum / N;
}

double simulateNumberOfExploresCached(int d, int k)
{
    static std::map<std::pair<int, int>, int> simCache;
    auto it = simCache.find(std::make_pair(d, k));
    if (it == simCache.end())
    {
        double res = simulateNumberOfExplores(d, k);
        simCache[std::make_pair(d, k)] = res;
        return res;
    }

    return it->second;
}

double areaPoints(double estimatedAmount, int area)
{
    if (estimatedAmount <= 0)
        return 0;

    int a = std::ceil(estimatedAmount);
    return a / (1e-10 + simulateNumberOfExploresCached(area, a));
}

struct KnownArea
{
    P pos, size;
    int amount;
    double estimatedAmount;
    KnownArea *parent = nullptr;

    std::vector<KnownArea *> children;

    int area() const {
        return size.x * size.y;
    }

    double areaPoints() const {
        return ::areaPoints(estimatedAmount, area());
    }

    bool isSingleCell() const
    {
        return size.x == 1 && size.y == 1;
    }
};

class Compare
{
public:
    bool operator() (const KnownArea *a, const KnownArea *b)
    {
        return a->areaPoints() < b->areaPoints();
    }
};


const int SPLIT_WID = 9;
const int SPLIT_H = 7;
const int SZ = 3500;

struct CashTask {
    std::string treasure;
    int depth;
};

void printStats();

std::atomic_int foundCols = 0;
std::atomic_int foundTh = 0;

enum class Mode
{
    EXPLORE, DIG, ALL
};

struct State
{
    std::set<int> coins;
    std::mutex mutex;
    BlockingQueue<KnownArea *> digPoints;
    BlockingQueue<CashTask> cashTasks;
    int totalScore = 0;

    State() : digPoints(), cashTasks(20) {
        /*digPoints.setLimitReachedFunc([this](int cnt){
            //LOG("DIGP " << cnt);
            if (cnt > 0)
                changeMode(Mode::DIG);
            else {
                changeMode(Mode::EXPLORE);
            }
            }, 2000);*/
    }

    Mode mode = Mode::EXPLORE;
    std::mutex modeMutex;
    std::condition_variable modeChanged;

    void waitForMode(Mode expectedMode)
    {
        /*while (isRunning) {
            std::unique_lock lock(modeMutex);
            if (mode == expectedMode || mode == Mode::ALL)
                return;

            modeChanged.wait(lock);
        }*/
    }

    void changeMode(Mode newMode)
    {
        {
            std::unique_lock lock(modeMutex);
            if (mode == Mode::ALL)
                return;

            if (newMode == Mode::DIG && mode != Mode::EXPLORE)
                return;

            if (newMode == Mode::EXPLORE && mode != Mode::DIG)
                return;

            //LOG("SET MODE " << (int) newMode);
            mode = newMode;
        }

        modeChanged.notify_all();
    }

    std::vector<int> getCoins()
    {
        std::lock_guard<std::mutex> g(mutex);

        std::vector<int> res;

        int num = 0;
        /*if (coins.size() >= 21)
            num = 21;
        else*/ /*if (coins.size() >= 1)
            num = 1;*/

        for (int i = 0; i < num; ++i) {
            auto it = coins.begin();
            int coin = *it;
            coins.erase(it);
            res.push_back(coin);
        }

        return res;
    }

    void addCoins(const std::vector<int> &wallet)
    {
        bool isPrintStats = false;
        {
            std::lock_guard<std::mutex> g(mutex);

            int oldScore = totalScore;
            totalScore += wallet.size();
            coins.insert(wallet.begin(), wallet.end());

            if (oldScore < 700000 && totalScore >= 700000)
                isPrintStats = true;
        }

        /*if (isPrintStats)
            printStats();*/
    }

    int getTotalCoins()
    {
        std::lock_guard<std::mutex> g(mutex);
        return coins.size();
    }

    void digArea(KnownArea &a) {
        digPoints.push(&a);
        foundCols++;
        foundTh += a.amount;
    }

    void asyncCash(const std::string treasure, int depth)
    {
        CashTask t = {
                .treasure = treasure,
                .depth = depth
        };

        cashTasks.push(t);
    }
};

const char *getHost()
{
    const char *host = std::getenv("ADDRESS");
    if (!host)
        host = "localhost";

    return host;
}

void setupTimeouts(httplib::Client &cli)
{
    cli.set_connection_timeout(60, 0); // 60s
    cli.set_read_timeout(60, 0); // 60 seconds
    cli.set_write_timeout(60, 0); // 60 seconds
}

void cash(State &state, httplib::Client &cli, const std::string &t, int depth)
{
    for (int i = 0; i < 5; ++i) {
        auto wallet = postCash(cli, t);
        if (wallet.has_value()) {
            updateThCost(depth, wallet->size());
            state.addCoins(*wallet);
            break;
        }
    }
}

struct Casher {
    httplib::Client cli;
    State &state;

    Casher(State &state) : state(state), cli(getHost(), 8000) {
        setupTimeouts(cli);
    }

    void run()
    {
        CpuTimeMeasure tm("C");
        try
        {
            while(isRunning)
            {

                CashTask t;
                if (!state.cashTasks.pop(t))
                    return;

                cash(state, cli, t.treasure, t.depth);
            }
        }
        catch (const std::runtime_error &err) {
        }
    }
};

const int MAX_LICENSES = 10;

struct LicenseUsage
{
    int remainingDigs;
    int inFlyDigs;
};

struct LicensePool
{
    State &state;
    std::mutex mutex;
    int pendingRequests = 0;
    std::map<int, LicenseUsage> licenses;
    std::condition_variable canPostForLicense;

    LicensePool(State &state) : state(state) {}

    void refreshLicense(httplib::Client &cli)
    {
        bool requestLicense = false;
        {
            std::unique_lock lock(mutex);
            if (pendingRequests + licenses.size() < MAX_LICENSES) {
                requestLicense = true;
                pendingRequests++;
            }
        }

        if (requestLicense)
        {
            std::optional<License> license;
            while (!license.has_value() || license->digUsed >= license->digAllowed) {
                license = postLicense(cli, state.getCoins());
            }

            {
                std::unique_lock lock(mutex);
                LicenseUsage &u = licenses[license->id];
                u.inFlyDigs = 0;
                u.remainingDigs = license->digAllowed;
                pendingRequests--;
            }
            canPostForLicense.notify_all();
        }
    }

    int getLicenseId(httplib::Client &cli)
    {
        bool requestLicense = false;
        while (!requestLicense)
        {
            std::unique_lock lock(mutex);

            if (pendingRequests + licenses.size() < MAX_LICENSES) {
                requestLicense = true;
                pendingRequests++;
            }
            else
            {
                for (auto &p : licenses)
                {
                    if (p.second.remainingDigs > p.second.inFlyDigs)
                    {
                        p.second.inFlyDigs++;
                        return p.first;
                    }
                }

                for (auto &p : licenses)
                {
                    if (p.second.remainingDigs >= p.second.inFlyDigs)
                    {
                        p.second.inFlyDigs++;
                        return p.first;
                    }
                }

                canPostForLicense.wait(lock);
            }
        }


        std::optional<License> license;
        while (!license.has_value() || license->digUsed >= license->digAllowed) {
            license = postLicense(cli, state.getCoins());
        }

        {
            std::unique_lock lock(mutex);
            LicenseUsage &u = licenses[license->id];
            u.inFlyDigs = 1;
            u.remainingDigs = license->digAllowed;
            pendingRequests--;
        }
        canPostForLicense.notify_all();

        return license->id;
    }

    void licenseConsumed(int license)
    {
        std::unique_lock lock(mutex);
        auto it = licenses.find(license);
        if (it != licenses.end()) {
            it->second.inFlyDigs--;
            it->second.remainingDigs--;
            if (it->second.remainingDigs == 0) {
                licenses.erase(it);
                canPostForLicense.notify_one();
            }
        }
    }
};

struct Digger {
    std::optional<int> licenseId;
    State &state;
    LicensePool &licensePool;
    httplib::Client cli;

    Digger(State &state, LicensePool &licensePool) : state(state), licensePool(licensePool), cli(getHost(), 8000) {
        setupTimeouts(cli);
    }

    void run()
    {
        CpuTimeMeasure tm("D");
        try
        {
            while(isRunning)
            {
                licensePool.refreshLicense(cli);

                state.waitForMode(Mode::DIG);

                KnownArea *a = nullptr;
                if (!state.digPoints.pop(a))
                    return;

                int depth = 1;
                int left = a->amount;
                while (depth <= 10 && left > 0) {
                    refreshLicense();

                    auto treasures = postDig(cli, *licenseId, {.x = a->pos.x, .y = a->pos.y}, depth);
                    licensePool.licenseConsumed(*licenseId);
                    licenseId.reset();

                    depth++;
                    if (treasures.has_value()) {
                        left -= treasures->size();
                        for (const std::string &t : *treasures) {
                            if (state.getTotalCoins() < 1000)
                            {
                                cash(state, cli, t, depth - 1);
                            }
                            else
                            {
                                if (depth > 3)
                                    state.asyncCash(t, depth - 1);
                            }
                        }
                    }
                }
            }
        }
        catch (const std::runtime_error &err) {
        }
    }

    void refreshLicense()
    {
        if (!licenseId.has_value())
            licenseId = licensePool.getLicenseId(cli);
    }
};

double hSplitCost(P size)
{
    if (size.x <= 1)
        return 10000;

    int w = size.x / 2;
    int w2 = size.x - w;

    return exploreCost(w * size.y) + exploreCost(w2 * size.y);
}

double vSplitCost(P size)
{
    if (size.y <= 1)
        return 10000;

    int h = size.y / 2;
    int h2 = size.y - h;

    return exploreCost(size.x * h) + exploreCost(size.x * h2);
}

struct AreaMap {
    std::vector<std::unique_ptr<KnownArea>> allAreas;
    std::vector<KnownArea *> notInvestigatedAreas;

    std::mutex qMutex;
    std::priority_queue<KnownArea *, std::vector<KnownArea *>, Compare> q;

    std::mutex totalKnownAreaMutex;
    int totalKnownAreaAmount = 0;
    int totalKnownArea = 0;

    BlockingQueue<KnownArea *> areasToExplore;
    std::mutex pendingMutex;
    int pending = 0;
    std::condition_variable exploreFinished;

    AreaMap() : areasToExplore(20) {
        for (int y = 0; y < SZ; y += SPLIT_H)
        {
            if (y + SPLIT_H > SZ)
                continue;

            for (int x = 0; x < SZ; x += SPLIT_WID)
            {
                if (x + SPLIT_WID > SZ)
                    continue;

                KnownArea *a = new KnownArea();
                a->pos.x = x;
                a->pos.y = y;
                a->size.x = SPLIT_WID;
                a->size.y = SPLIT_H;
                a->amount = -1;
                allAreas.emplace_back(a);
                notInvestigatedAreas.emplace_back(a);
            }
        }

        auto rng = std::default_random_engine {};
        std::shuffle(notInvestigatedAreas.begin(), notInvestigatedAreas.end(), rng);
    }

    void split(KnownArea &a)
    {
        double A = a.size.x * a.size.y;

        assert(a.children.empty());

        // split
        if (hSplitCost(a.size) > vSplitCost(a.size))
        {
            int h = a.size.y / 2;

            KnownArea *c1 = new KnownArea();
            c1->parent = &a;
            c1->pos.x = a.pos.x;
            c1->pos.y = a.pos.y;
            c1->size.x = a.size.x;
            c1->size.y = h;
            c1->amount = -1;

            double C1A = c1->size.x * c1->size.y;
            c1->estimatedAmount = a.amount * C1A / A;

            KnownArea *c2 = new KnownArea();
            c2->parent = &a;
            c2->pos.x = a.pos.x;
            c2->pos.y = a.pos.y + h;
            c2->size.x = a.size.x;
            c2->size.y = a.size.y - h;
            c2->amount = -1;

            double C2A = c2->size.x * c2->size.y;
            c2->estimatedAmount = a.amount * C2A / A;

            if (exploreCost(c1->size) <= exploreCost(c2->size))
            {
                a.children.push_back(c1);
                a.children.push_back(c2);
            }
            else
            {
                a.children.push_back(c2);
                a.children.push_back(c1);
            }

        } else {
            int w = a.size.x / 2;

            KnownArea *c1 = new KnownArea();
            c1->parent = &a;
            c1->pos.x = a.pos.x;
            c1->pos.y = a.pos.y;
            c1->size.x = w;
            c1->size.y = a.size.y;
            c1->amount = -1;

            double C1A = c1->size.x * c1->size.y;
            c1->estimatedAmount = a.amount * C1A / A;


            KnownArea *c2 = new KnownArea();
            c2->parent = &a;
            c2->pos.x = a.pos.x + w;
            c2->pos.y = a.pos.y;
            c2->size.x = a.size.x - w;
            c2->size.y = a.size.y;
            c2->amount = -1;

            double C2A = c2->size.x * c2->size.y;
            c2->estimatedAmount = a.amount * C2A / A;

            if (exploreCost(c1->size) <= exploreCost(c2->size))
            {
                a.children.push_back(c1);
                a.children.push_back(c2);
            }
            else
            {
                a.children.push_back(c2);
                a.children.push_back(c1);
            }
        }

        {
            std::lock_guard<std::mutex> g(qMutex);
            q.push(a.children[0]);
        }
    }

    void addKnownAreaStats(KnownArea &a)
    {
        std::lock_guard<std::mutex> g(totalKnownAreaMutex);
        totalKnownAreaAmount += a.amount;
        totalKnownArea += a.area();
    }

    void estimateAmountByDensity(KnownArea &a)
    {
        std::lock_guard<std::mutex> g(totalKnownAreaMutex);
        a.estimatedAmount = (double) totalKnownAreaAmount / ((double) totalKnownArea + 1.e-10) * a.area();
    }

    void pushInitialAreas()
    {
        int i = 0;
        while (isRunning) {
            if (notInvestigatedAreas.empty())
                break;

            if (totalKnownAreaAmount && i > 50)
                break;

            ++i;

            KnownArea *a = *notInvestigatedAreas.rbegin();
            notInvestigatedAreas.pop_back();

            {
                std::lock_guard<std::mutex> g(pendingMutex);
                pending++;
            }
            areasToExplore.push(a);
        }
    }

    bool push()
    {
        KnownArea *res = nullptr;
        {
            std::lock_guard<std::mutex> g(qMutex);
            if (!q.empty() || !notInvestigatedAreas.empty()) {
                if (!q.empty() && !notInvestigatedAreas.empty()) {
                    KnownArea *a2 = *notInvestigatedAreas.rbegin();
                    estimateAmountByDensity(*a2);
                    KnownArea *a = q.top();

                    if (a->areaPoints() > a2->areaPoints()) {
                        q.pop();
                        res = a;
                    } else {
                        notInvestigatedAreas.pop_back();
                        res = a2;
                    }
                } else if (!q.empty()) {
                    KnownArea *a = q.top();
                    q.pop();
                    res = a;
                } else {
                    KnownArea *a = *notInvestigatedAreas.rbegin();
                    notInvestigatedAreas.pop_back();
                    res = a;
                }
            }
        }

        if (res)
        {
            {
                std::lock_guard<std::mutex> g(pendingMutex);
                pending++;
            }
            areasToExplore.push(res);
            return true;
        }
        else
        {
            std::unique_lock lock(pendingMutex);
            if (pending > 0)
            {
                //LOG("WAIT");
                exploreFinished.wait(lock);
                return true;
            }
            return false;
        }
    }
};

struct Explorer {
    State &state;
    AreaMap &areaMap;
    httplib::Client cli;

    Explorer(State &state, AreaMap &areaMap) : state(state), areaMap(areaMap), cli(getHost(), 8000) {
        setupTimeouts(cli);
    }

    void explore(KnownArea &a)
    {
        while (isRunning) {
            Area area{
                    .pos = a.pos,
                    .size = a.size
            };

            std::optional<int> amountOpt = postExplore(cli, area);
            if (!amountOpt.has_value())
                continue;

            a.amount = *amountOpt;
            a.estimatedAmount = a.amount;

            if (a.parent)
            {
                int siblingAmount = a.parent->amount - *amountOpt;
                KnownArea *sibling = a.parent->children[1];

                sibling->amount = siblingAmount;
                sibling->estimatedAmount = siblingAmount;
                if (sibling->amount)
                {
                    if (sibling->isSingleCell())
                        state.digArea(*sibling);
                    else
                        areaMap.split(*sibling);
                }
            }
            else
            {
                areaMap.addKnownAreaStats(a);
            }

            if (a.amount == 0) {
                break;
            }

            if (a.isSingleCell()) {
                state.digArea(a);
            }
            else
            {
                areaMap.split(a);
            }

            break;
        }
    }

    void run()
    {
        CpuTimeMeasure tm("E");
        while(isRunning)
        {
            KnownArea *a = nullptr;
            if (!areaMap.areasToExplore.pop(a))
                return;

            state.waitForMode(Mode::EXPLORE);

            try
            {
                explore(*a);
            }
            catch (const std::runtime_error &err) {
            }

            {
                std::lock_guard<std::mutex> g(areaMap.pendingMutex);
                areaMap.pending--;
            }
            areaMap.exploreFinished.notify_one();
        }
    }
};

const int DIGGER_N = 60;
const int EXPLORERS_N = 1;
const int CASHER_N = 5;

AreaMap areaMap;
State state;
LicensePool licensePool(state);

void closeAll()
{
    //LOG("CloseAll");
    areaMap.areasToExplore.close();
    state.digPoints.close();
    state.modeChanged.notify_all();
}

void printStats()
{
    {
        std::lock_guard<std::mutex> g(timeStatsMutex);
        for (auto &p : _timeStats) {
            LOG("TT " << p.first << ": " << p.second);
        }
    }

    {
        std::lock_guard<std::mutex> g(cpuTimeStatsMutex);
        for (auto &p : _cpuTimeStats) {
            LOG("CT " << p.first << ": " << p.second);
        }
    }

    LOG("l502 " << errLicense502 << "/" << errLicense502WithCoin << " | 504 " << errLicense504 << "/" << errLicense504WithCoin << " | 200 " << license200 << "/" << license200WithCoin << " | c503 " << errCash503 << " | c200 " << cash200 << " TR " << totalReqs);

    {
        std::ostringstream oss;
        oss << "COST ";
        for (auto &p : thCost) {
            oss << p.first << ":" << p.second.c << "/" << p.second.n << " ";
        }

        LOG(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "LCS ";
        for (auto &p : licenseDigsPerCoins) {
            oss << p.first << ":";

            for (auto d : p.second)
                oss << d << " ";

            oss << "|";
        }

        LOG(oss.str());
    }

    LOG("SCORE " << state.totalScore << " C " << foundCols << " T " << foundTh << " E "<< (expCost / 4) << " DE " << digErrors);
}

int main() {
    std::cout << "START" << std::endl;

    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    for (int i = 1; i <= 10; ++i)
    {
        thCost[i];
    }

    httplib::Client cli(getHost(), 8000);
    setupTimeouts(cli);

    std::vector<std::unique_ptr<Digger>> diggers;
    diggers.reserve(DIGGER_N);

    std::vector<std::thread> diggerThreads;
    diggerThreads.reserve(DIGGER_N);

    std::vector<std::unique_ptr<Explorer>> explorers;
    explorers.reserve(EXPLORERS_N);

    std::vector<std::thread> explorersThreads;
    explorersThreads.reserve(EXPLORERS_N);

    std::vector<std::unique_ptr<Casher>> cashers;
    cashers.reserve(CASHER_N);

    std::vector<std::thread> cashersThreads;
    cashersThreads.reserve(CASHER_N);

    std::thread areaPusher;

    try {
        /*for (int i = 0; i < CASHER_N; ++i) {
            cashers.emplace_back(new Casher(state));
            cashersThreads.emplace_back(&Casher::run, cashers[i].get());
        }

        for (int i = 0; i < DIGGER_N; ++i) {
            diggers.emplace_back(new Digger(state, licensePool));
            diggerThreads.emplace_back(&Digger::run, diggers[i].get());
        }*/

        for (int i = 0; i < EXPLORERS_N; ++i) {
            explorers.emplace_back(new Explorer(state, areaMap));
            explorersThreads.emplace_back(&Explorer::run, explorers[i].get());
        }

        areaPusher = std::thread([](){
            CpuTimeMeasure tm("A");
            try {
                areaMap.pushInitialAreas();
                while (areaMap.push()) {}
            }
            catch (const std::runtime_error &err)
            {
                LOG("ERROR " << err.what());
            }
            LOG("PUSH finish");
        });

        bool modeChanged = false;
        while (isRunning)
        {
            /*std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            int s = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (s > 550 && !modeChanged)
            {
                state.digPoints.set_limit(50);
                state.cashTasks.set_limit(50);

                state.changeMode(Mode::ALL);
                modeChanged = true;

                LOG("Mode change to ALL");
            }*/

            std::this_thread::sleep_for(std::chrono::seconds(1));
            //LOG("T " << s);
        }
    }
    catch (const std::runtime_error &err)
    {
        LOG("ERROR " << err.what());
    }

    if (allReq.size() > 1000)
    {
        std::ostringstream oss;
        int s = allReq.size() - 1000;
        ReqT &rb = allReq[s];

        for (int i = s; i < s + 500; ++i)
        {
            ReqT &r = allReq[i];
            long ts = std::chrono::duration_cast<std::chrono::microseconds>(r.s - rb.s).count();
            long te = std::chrono::duration_cast<std::chrono::microseconds>(r.e - rb.s).count();

            oss << "|" << r.key << " " << ts << " " << te;
        }

        LOG(oss.str());
    }

    state.digPoints.close();
    state.cashTasks.close();
    areaMap.areasToExplore.close();
    for (auto &th : diggerThreads)
        th.join();

    for (auto &th : cashersThreads)
        th.join();

    for (auto &th : explorersThreads)
        th.join();

    areaPusher.join();

    printStats();

    return 0;
}