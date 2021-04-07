#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <utility>
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <future>
#include <functional>
#include <set>
#include <queue>
#include <fstream>
#include <random>
#include "simdjson.h"

//#define USE_HIST 1
//#define USE_TH_COST 1

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

using work_guard_type = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

#define LOG(x)  std::cout << x << std::endl;

std::atomic<bool> isRunning = true;
net::io_context ioc;
tcp::resolver::results_type resolveResults;

static int MAX_SMLT_CASH_REQS = 24;
static int MAX_SMLT_DIG_REQS = 24;
static int MAX_SMLT_EXPL_REQS = 24;
static int WAIT_LICENSE_MS = 30;
static int THREADS = 5;
static std::atomic_int MIN_CASH_DEPTH = 3;


const int SPLIT_WID = 255;

std::atomic<int> totalReqs = 0;
std::map<std::string, int> reqCnt;

void setMinSeed(int seed)
{
    if (seed >= 4)
    {
        MIN_CASH_DEPTH = 4;
        LOG("MC4 " << totalReqs);
    }
}

struct TimeStat
{
    double t = 0;
    int count = 0;
    double cost = 0;
    std::map<int, int> hist;
};

std::ostream &operator << (std::ostream &s, const TimeStat &st) {
    s << st.t << " " << st.count << " " << (st.t / (st.count + 1e-6));
    s << " H ";
    if (st.hist.size() < 22) {
        for (auto &p : st.hist) {
            s << "| " << p.first << "," << p.second;
        }
    }
    else
    {
        int min = st.hist.begin()->first;
        int max = st.hist.rbegin()->first;

        s << " max " << max << " | ";

        int cnt = st.hist.size();
        auto it1 = st.hist.begin();
        auto it2 = st.hist.end();
        std::advance(it1, cnt / 20);
        std::advance(it2, -cnt / 20);

        int vs = it1->first;
        int ve = it2->first;

        int sumMin = 0;
        int sumMax = 0;

        std::map<int, int> m;
        int delta = (ve - vs) / 20;
        if (delta < 1)
            delta = 1;
        ve = vs + delta * 20;
        for (auto p : st.hist)
        {
            if (p.first < vs)
            {
                sumMin += p.second;
            }
            else if (p.first >= ve)
            {
                sumMax += p.second;
            }
            else
            {
                m[((p.first - vs) / delta) * delta + vs] += p.second;
            }
        }

        s << min << "," << sumMin;
        for (auto p : m)
        {
            s << "|" << p.first << "," << p.second;
        }
        s << "|" << ve << "," << sumMax;
    }
    return s;
}

std::map<std::string, TimeStat> _timeStats;
std::mutex timeStatsMutex;

std::map<int, std::map<int, int>> thCost;
std::mutex thCostMutex;
void updateThCost(int depth, int coinsN, int len)
{
    std::lock_guard<std::mutex> g(thCostMutex);
    thCost[depth][coinsN]++;
}

struct TimeStatUpd {
    std::string key, origKey;
    std::chrono::steady_clock::time_point s;
    double cost = 0;
    bool isReq = false;

    void start(const std::string &key)
    {
        this->key = key;
        this->origKey = key;
        s = std::chrono::steady_clock::now();

        if (isReq)
        {
            std::lock_guard<std::mutex> g(timeStatsMutex);
            reqCnt[origKey]++;
        }
    }

    void end() const
    {
        std::chrono::steady_clock::time_point c_end = std::chrono::steady_clock::now();
        double dt = std::chrono::duration_cast<std::chrono::nanoseconds>(c_end - s).count() * 0.000001;

        std::lock_guard<std::mutex> g(timeStatsMutex);
        TimeStat &total = _timeStats[key];
        total.t += dt;
        total.count++;
        total.cost += cost;

#ifdef USE_HIST
        total.hist[(int) (dt*10)]++;
#endif

        if (isReq)
        {
            reqCnt[origKey]--;
        }
    }

    void end(int code)
    {
        key = key + std::to_string(code);
        end();
    }
};

std::map<std::string, TimeStat> _cpuTimeStats;
std::mutex cpuTimeStatsMutex;

TimeStat & getCpuTimeStat(const std::string &key)
{
    std::lock_guard<std::mutex> g(cpuTimeStatsMutex);
    return _cpuTimeStats[key];
}

struct CpuTimeMeasure
{
    explicit CpuTimeMeasure(const std::string &key) : total(getCpuTimeStat(key)) {
        c_start = std::clock();
    }

    ~CpuTimeMeasure()
    {
        std::clock_t c_end = std::clock();
        double dt = 1000.0 * (c_end - c_start) / CLOCKS_PER_SEC;

        std::lock_guard<std::mutex> g(cpuTimeStatsMutex);
        total.t += dt;
        total.count++;
#ifdef USE_HIST
        total.hist[(int)(dt * 10)]++;
#endif
    }

    TimeStat &total;
    std::clock_t c_start;
};

const char *getHost()
{
    const char *host = std::getenv("ADDRESS");
    if (!host)
        host = "localhost";

    return host;
}

const char *PORT = "8000";

struct HttpResp {
    int httpCode = -1;
    std::string body;
};

template <typename T>
struct Resp {
    std::exception_ptr exception;
    T data;
};

typedef std::function<void (Resp<HttpResp>)> HttpResultFunc;

struct Req : public std::enable_shared_from_this<Req> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_; // (Must persist between reads)
    http::request<http::string_body > req_;
    http::response<http::string_body> res_;
    HttpResultFunc resultFunc;
    TimeStatUpd timeStatUpd;

    explicit
    Req(HttpResultFunc &&resultFunc): stream_(net::make_strand(ioc)), resultFunc(std::move(resultFunc)) {
    }

    void run(const char *path, const std::string &body)
    {
        req_.version(11);
        req_.method(http::verb::post);
        req_.set(http::field::content_type, "application/json");
        req_.target(path);
        req_.set(http::field::host, getHost());
        req_.set(http::field::user_agent, "ud1 hlc");
        req_.body() = body;
        req_.prepare_payload();

        timeStatUpd.isReq = true;
        timeStatUpd.start(path);
        stream_.async_connect(
                resolveResults,
                beast::bind_front_handler(
                        &Req::on_connect,
                        shared_from_this()));
    }

    void on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type&)
    {
        if(ec) {
            fail(ec);
            return;
        }

        // Set a timeout on the operation
        stream_.expires_after(std::chrono::seconds(30));

        //LOG("Write " << req_.body());
        // Send the HTTP request to the remote host
        http::async_write(stream_, req_,
                          beast::bind_front_handler(
                                  &Req::on_write,
                                  shared_from_this()));
    }

    void on_write(
            beast::error_code ec,
            std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if(ec) {
            fail(ec);
            return;
        }

        // Receive the HTTP response
        http::async_read(stream_, buffer_, res_,
                         beast::bind_front_handler(
                                 &Req::on_read,
                                 shared_from_this()));
    }

    void on_read(
            beast::error_code ec,
            std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
        {
            fail(ec);
            return;
        }

        // Write the message to standard out
        //std::cout << res_ << std::endl;

        Resp<HttpResp> resp;
        resp.data.httpCode = (int) res_.result();
        resp.data.body = res_.body();

        timeStatUpd.end(resp.data.httpCode);
        resultFunc(std::move(resp));

        // Gracefully close the socket
        stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    }

    void fail(const beast::error_code &ec) {
        isRunning = false;
        //LOG("ERR " << ec.message());
        try {
            throw std::runtime_error(ec.message());
        }
        catch (...) {
            try {
                Resp<HttpResp> resp;
                resp.exception = std::current_exception();
                resultFunc(std::move(resp));
            } catch (...) {}
        }
    }
};

/*
 Costs
Total 1.2kk points
exp 1*1 = 1 point
cash = 20 points
dig = 2 * (1 + (depth-1)/11.25)
 * */

void intReq()
{
    totalReqs++;

#ifdef MAX_TOTAL_REQS
    if (totalReqs > MAX_TOTAL_REQS)
    {
        isRunning = false;
    }
#endif
}

void post(const char *path, const std::string &body, HttpResultFunc &&resultFunc)
{
    intReq();
    auto req = std::make_shared<Req>(std::move(resultFunc));
    req->run(path, body);
}

void postWithRetry(const char *path, const std::string &body, const std::function<void (const std::string &body)>& result, const std::function<void ()>& onError)
{
    intReq();

    if (isRunning) {
        auto req = std::make_shared<Req>([=](const Resp<HttpResp>& r) {
            if (!r.exception)
            {
                if (r.data.httpCode == 200) {
                    result(r.data.body);
                }
                else if (r.data.httpCode < 500) {
                    //LOG("ERR " << path << " " << body << " " << r.data.httpCode << " " << r.data.body);
                    onError();
                }
                else // retry
                {
                    postWithRetry(path, body, result, onError);
                }
            }
        });
        req->run(path, body);
    }
}

void postWithRetry(const char *path, const std::string &body, const std::function<void (const std::string &body)>& result)
{
    intReq();

    if (isRunning) {
        auto req = std::make_shared<Req>([=](const Resp<HttpResp>& r) {
            if (!r.exception)
            {
                if (r.data.httpCode == 200) {
                    result(r.data.body);
                }
                else if (r.data.httpCode == 429)
                {
                    postWithRetry(path, body, result);
                }
                else if (r.data.httpCode < 500) {
                    LOG("ERR " << path << " " << body << " " << r.data.httpCode << " " << r.data.body);
                }
                else // retry
                {
                    postWithRetry(path, body, result);
                }
            }
        });
        req->run(path, body);
    }
}

/////////////////////
struct P {
    int x, y;
};

struct Area {
    P pos, size;
};

struct License {
    int64_t id;
    int64_t digAllowed;
    int64_t digUsed;
};

//////////////////////////////////////////////////////////

bool isCashInRange(int seed, int depth, int coins)
{
    switch(seed)
    {
        case 1:
        {
            switch (depth)
            {
                case 1: return 3 <= coins && coins <= 5;
                case 2: return 7 <= coins && coins <= 11;
                case 3: return 12 <= coins && coins <= 18;
                case 4: return 15 <= coins && coins <= 23;
                case 5: return 17 <= coins && coins <= 27;
                case 6: return 18 <= coins && coins <= 30;
                case 7: return 20 <= coins && coins <= 32;
                case 8: return 21 <= coins && coins <= 35;
                case 9: return 22 <= coins && coins <= 36;
                case 10: return 23 <= coins && coins <= 37;
            }
        }
        case 2:
        {
            switch (depth)
            {
                case 1: return 3 <= coins && coins <= 5;
                case 2: return 7 <= coins && coins <= 11;
                case 3: return 12 <= coins && coins <= 18;
                case 4: return 18 <= coins && coins <= 28;
                case 5: return 21 <= coins && coins <= 33;
                case 6: return 23 <= coins && coins <= 37;
                case 7: return 25 <= coins && coins <= 41;
                case 8: return 27 <= coins && coins <= 43;
                case 9: return 28 <= coins && coins <= 46;
                case 10: return 29 <= coins && coins <= 47;
            }
        }
        case 3:
        {
            switch (depth)
            {
                case 1: return 3 <= coins && coins <= 5;
                case 2: return 7 <= coins && coins <= 11;
                case 3: return 12 <= coins && coins <= 18;
                case 4: return 18 <= coins && coins <= 28;
                case 5: return 25 <= coins && coins <= 41;
                case 6: return 28 <= coins && coins <= 46;
                case 7: return 30 <= coins && coins <= 50;
                case 8: return 33 <= coins && coins <= 53;
                case 9: return 34 <= coins && coins <= 56;
                case 10: return 36 <= coins && coins <= 58;
            }
        }
        case 4:
        {
            switch (depth)
            {
                case 1: return 3 <= coins && coins <= 5;
                case 2: return 7 <= coins && coins <= 11;
                case 3: return 12 <= coins && coins <= 18;
                case 4: return 18 <= coins && coins <= 28;
                case 5: return 25 <= coins && coins <= 41;
                case 6: return 34 <= coins && coins <= 56;
                case 7: return 37 <= coins && coins <= 61;
                case 8: return 39 <= coins && coins <= 65;
                case 9: return 42 <= coins && coins <= 68;
                case 10: return 43 <= coins && coins <= 71;
            }
        }
        case 5:
        {
            switch (depth)
            {
                case 1: return 3 <= coins && coins <= 5;
                case 2: return 7 <= coins && coins <= 11;
                case 3: return 12 <= coins && coins <= 18;
                case 4: return 18 <= coins && coins <= 28;
                case 5: return 25 <= coins && coins <= 41;
                case 6: return 34 <= coins && coins <= 56;
                case 7: return 45 <= coins && coins <= 75;
                case 8: return 48 <= coins && coins <= 80;
                case 9: return 51 <= coins && coins <= 83;
                case 10: return 53 <= coins && coins <= 87;
            }
        }
    }

    LOG("UNEXP COINS " << seed << " " << depth << " " << coins);
    return false;
}

struct SeedDetect {
    std::set<int> seeds = {1, 2, 3, 4, 5};
    std::mutex mutex;

    void check(int coins, int depth)
    {
        std::lock_guard<std::mutex> g(mutex);

        if (seeds.size() > 1)
        {
            int minSeed = *seeds.begin();
            for (auto it{seeds.begin()}, end{seeds.end()}; it != end; )
            {
                if (!isCashInRange(*it, depth, coins))
                {
                    it = seeds.erase(it);
                }
                else {
                    ++it;
                }
            }

            if (seeds.size() == 1)
            {
                LOG("SEED " << *seeds.begin());
            }
            else if (seeds.empty())
            {
                LOG("SEED NOT FOUND " << depth << " " << coins);
            }

            if (!seeds.empty())
            {
                int newMinSeed = *seeds.begin();
                if (newMinSeed != minSeed) {
                    setMinSeed(newMinSeed);
                }
            }
        }
    }
};

SeedDetect seedDetect;

//////////////////////////////////////////////////////////


double exploreCost(int area)
{
    if (area <= 3)
        return 1;

    double res = 1;
    for (int i = 4; i <= area; i *= 2)
    {
        res += 1;
    }

    return res;
}

double exploreCost(P area)
{
    return exploreCost(area.x * area.y);
}

double digCost(int depth)
{
    //return 2.0 * (1.0 + (depth - 1) / 11.25);
    return 2.1 + 0.18*(depth - 1);
}

//////////////////////////////////////////////////////////

template<class T>
using ParseFunc = T (const HttpResp &);

int parseExplore(const std::string &resp)
{
    simdjson::dom::parser parser;
    auto r = parser.parse(resp);
    int64_t res = r["amount"];
    return res;
};

void postExplore(const Area &area, const std::function<void (int)>& result)
{
    std::ostringstream reqOss;
    reqOss << "{\"posX\":" << area.pos.x << ",\"posY\":" << area.pos.y << ",\"sizeX\":" << area.size.x << ",\"sizeY\":" << area.size.y << "}";

    auto reqStr = reqOss.str();

    TimeStatUpd upd;
    upd.start("exp_" + std::to_string(area.size.x) + "_" + std::to_string(area.size.y));
    upd.cost = exploreCost(area.size.x * area.size.y);

    postWithRetry("/explore", reqStr, [result, upd](const std::string &s){
        upd.end();
        result(parseExplore(s));
    });
}

License parseLicense(const std::string &res)
{
    simdjson::dom::parser parser;
    auto resp = parser.parse(res);

    License license {
            .id = resp["id"].get_int64(),
            .digAllowed = resp["digAllowed"].get_int64(),
            .digUsed = resp["digUsed"].get_int64()
    };
    return license;
}

void postLicense(const std::function<void (License)>& result, const std::function<void ()>& onError)
{
    postWithRetry("/licenses", "[]", [result](const std::string &s){
        result(parseLicense(s));
    }, onError);
}

void postDig(int licenseId, P p, int depth, const std::function<void (std::vector<std::string>)>& result)
{
    std::ostringstream reqOss;
    reqOss << "{\"licenseID\":" << licenseId << ",\"posX\":" << p.x << ",\"posY\":" << p.y << ",\"depth\":" << depth << "}";

    auto reqStr = reqOss.str();

    TimeStatUpd upd;
    upd.start("dig_" + std::to_string(depth));
    upd.cost = digCost(depth);

    post("/dig", reqStr, [=](const Resp<HttpResp> &resp){
        if (!resp.exception) {
            //LOG("DIG RES " << resp.data.httpCode << " " << resp.data.body);
            if (resp.data.httpCode == 404) {
                upd.end();
                result(std::vector<std::string>());
                return;
            }

            if (resp.data.httpCode != 200) {
                if (resp.data.httpCode >= 500) {
                    postDig(licenseId, p, depth, result);
                    return;
                }
                else
                {
                    LOG("DIG ERR " << resp.data.httpCode << " " << resp.data.body);
                    result(std::vector<std::string>());
                    return;
                }
            }

            upd.end();

            simdjson::dom::parser parser;
            auto res = parser.parse(resp.data.body);

            std::vector<std::string> treasures;

            for (auto v : res)
            {
                treasures.push_back(std::string(v));
            }

            result(treasures);
        }
    });
}

void postCash(const std::string &treasure, const std::function<void (std::vector<int> result)>& result)
{
    auto reqStr = std::string("\"") + treasure + std::string("\"");
    postWithRetry("/cash", reqStr, [result](const std::string &s){
        simdjson::dom::parser parser;
        auto resp = parser.parse(s);

        std::vector<int> r;

        for (int64_t c : resp)
            r.push_back(c);
        result(r);
    });
}

///////////////////////////////////////////////////

struct ReqLimiter {
    std::mutex mutex;
    int maxReqs;
    int currentReqs = 0;
    std::deque<std::pair<std::function<void (std::function<void ()>)>, std::function<void ()> > > delayedFuncs;

    ReqLimiter(int maxReqs) : maxReqs(maxReqs) {}

    void tryStart(const std::function<void (std::function<void ()>)>& func, const std::function<void ()>& onStartedFunc)
    {
        bool start = false;
        {
            std::lock_guard<std::mutex> g(mutex);
            if (currentReqs < maxReqs) {
                start = true;
                ++currentReqs;
            }
            else
            {
                delayedFuncs.emplace_back(func, onStartedFunc);
            }
        }

        if (start)
        {
            doStart(func, onStartedFunc);
        }
    }

private:
    void doStart(const std::function<void (std::function<void ()>)>& func, const std::function<void ()>& onStartedFunc)
    {
        ioc.post([this, func](){
            func([this](){
                std::pair<std::function<void (std::function<void ()>)>, std::function<void ()> > delayedFunc;
                {
                    std::lock_guard<std::mutex> g(mutex);
                    if (!delayedFuncs.empty())
                    {
                        delayedFunc = delayedFuncs.front();
                        delayedFuncs.pop_front();
                    }
                    else
                    {
                        currentReqs--;
                    }
                }

                if (delayedFunc.first)
                    doStart(delayedFunc.first, delayedFunc.second);
            });
        });
        onStartedFunc();
    }
};

struct KnownArea
{
    P pos, size;
    int amount;
    KnownArea *parent = nullptr;

    std::vector<KnownArea *> children;

    [[nodiscard]] int area() const {
        return size.x * size.y;
    }

    [[nodiscard]] double density() const {
        return (double) amount / (double) area();
    }

    [[nodiscard]] bool isSingleCell() const
    {
        return size.x == 1 && size.y == 1;
    }
};

class Compare
{
public:
    bool operator() (const KnownArea *a, const KnownArea *b)
    {
        return a->density() > b->density();
    }
};

const int SZ = 3500;

struct CashTask {
    std::string treasure;
    int depth;
};

std::atomic<int> foundCols = 0;

// Размер области на которую надо разбить область размером size в которой thCount сокровищь.
// В итоге будет две области размером res и (size - res)
int splitSize(int size, int thCount) // если возвращаем 0, то выкидываем вообще область и не эксплорим ее
{
    double dd = (double) size / (double) thCount;

    if (size > 70 && dd > 31)
        return 0;

    if (size > 110 && dd > 30)
        return 0;

    if (dd > 36)
        return 0;

    if (size <= 3)
        return 1;

    if (size >= 93 && (size / thCount) > 24)
    {
        return 31;
    }

    if (size >= 60 && (size / thCount) > 16)
    {
        return 15;
    }

    return 3;
}

struct AreaMap {
    std::vector<std::unique_ptr<KnownArea>> allAreas;
    std::vector<KnownArea *> notInvestigatedAreas;

    std::mutex qMutex;
    std::priority_queue<KnownArea *, std::vector<KnownArea *>, Compare> q;

    AreaMap() {
        for (int y = 0; y < SZ; y += 1)
        {
            if (y + 1 > SZ)
                continue;

            for (int x = 0; x < SZ; x += SPLIT_WID)
            {
                if (x + SPLIT_WID > SZ)
                    continue;

                KnownArea *a = new KnownArea();
                a->pos.x = x;
                a->pos.y = y;
                a->size.x = SPLIT_WID;
                a->size.y = 1;
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
        assert(a.children.empty());

        // split
        {
            int w = splitSize(a.size.x, a.amount);

            if (w == 0)
                return;

            KnownArea *c1 = new KnownArea();
            c1->parent = &a;
            c1->pos.x = a.pos.x;
            c1->pos.y = a.pos.y;
            c1->size.x = w;
            c1->size.y = a.size.y;
            c1->amount = -1;

            KnownArea *c2 = new KnownArea();
            c2->parent = &a;
            c2->pos.x = a.pos.x + w;
            c2->pos.y = a.pos.y;
            c2->size.x = a.size.x - w;
            c2->size.y = a.size.y;
            c2->amount = -1;

            a.children.push_back(c1);
            a.children.push_back(c2);
        }

        {
            std::lock_guard<std::mutex> g(qMutex);
            q.push(a.children[0]);
            allAreas.emplace_back(a.children[0]);
            allAreas.emplace_back(a.children[1]);
        }
    }

    KnownArea *nextArea()
    {
        std::lock_guard<std::mutex> g(qMutex);

        if (!q.empty())
        {
            KnownArea *a = q.top();
            q.pop();
            return a;
        }

        if (!notInvestigatedAreas.empty())
        {
            KnownArea *a = *notInvestigatedAreas.rbegin();
            notInvestigatedAreas.pop_back();
            return a;
        }

        return nullptr;
    }
};

const int MAX_LICENSES = 10;

struct LicenseUsage
{
    int remainingDigs;
    int inFlyDigs;
};

std::map<int, std::map<int, int>> digPoints;

struct State
{
    std::mutex mutex;
    ReqLimiter digLimiter = MAX_SMLT_DIG_REQS;
    ReqLimiter cashLimiter = MAX_SMLT_CASH_REQS;
    std::atomic_int totalScore = 0;

    AreaMap areaMap;

    void addCoins(const std::vector<int> &wallet)
    {
        totalScore += wallet.size();
    }

    void digArea(KnownArea &a, const std::function<void ()>& digStarted) {
        P pos = a.pos;
        int amount = a.amount;
        digLimiter.tryStart([this, pos, amount](const std::function<void ()>& finished){
            dig(pos, amount, 1, finished);
        }, digStarted);

        foundCols++;
    }

    // =============== Cash ==============

    void doCash(const std::string &treasure, int depth, const std::function<void ()>& finished)
    {
        int thLen = treasure.size();
        postCash(treasure, [this, depth, finished, thLen](const std::vector<int> &coins){
            seedDetect.check(coins.size(), depth);
#ifdef USE_TH_COST
            updateThCost(depth, coins.size(), thLen);
#endif
            addCoins(coins);
            finished();
        });
    }

    void cash(const std::string &treasure, int depth, const std::function<void ()>& started)
    {
        cashLimiter.tryStart([this, treasure, depth](const std::function<void ()>& finished){
            doCash(treasure, depth, finished);
        }, started);
    }

    void cash(std::vector<std::string> treasures, int depth, const std::function<void ()>& started)
    {
        std::string tr = treasures[treasures.size() - 1];
        treasures.pop_back();

        cash(tr, depth, [this, treasures, depth, started](){
            if (treasures.empty())
            {
                started();
            }
            else
            {
                cash(treasures, depth, started);
            }
        });
    }

    // =============== Dig ==============

    void dig(const P &p, int left, int depth, const std::function<void ()>& digFinished)
    {
        //LOG("DIG " << p.x << " " << p.y << " " << depth << " " << left);
        if (isRunning && depth <= 10 && left > 0) {
            getLicenseId([=](int licenseId) {
                postDig(licenseId, p, depth, [=](const std::vector<std::string>& treasures){
                    licenseConsumed(licenseId);

                    int thLeft = left;
                    thLeft -= treasures.size();

                    if (!treasures.empty() && depth >= MIN_CASH_DEPTH)
                    {
                        cash(treasures, depth, [=]() {
                            if (thLeft > 0) {
                                dig(p, thLeft, depth + 1, digFinished);
                            } else {
                                digFinished();
                            }
                        });
                    }
                    else
                    {
                        dig(p, thLeft, depth + 1, digFinished);
                    }
                });
            });
        }
        else
        {
            digFinished();
        }
    }

    // =========== Get license ===================

    std::mutex licenseMutex;
    int licensePendingRequests = 0;
    std::map<int, LicenseUsage> licenses;

    std::deque<std::function<void (int)>> licensePending;
    std::map<int, std::chrono::steady_clock::time_point> licenseReqs;
    int licenseReqSeq = 0;

    void spawnLicenseReqs()
    {
        std::vector<int> reqsToSpawnIds;
        {
            std::unique_lock lock(licenseMutex);

            auto t = std::chrono::steady_clock::now();
            auto expT = t - std::chrono::milliseconds(WAIT_LICENSE_MS);
            int realPendingReqs = 0;
            for (auto &p : licenseReqs)
            {
                if (p.second > expT)
                    realPendingReqs++;
            }

            int reqsToSpawn = MAX_LICENSES - realPendingReqs - licenses.size();
            for (int i = 0; i < reqsToSpawn; ++i) {
                int id = licenseReqSeq++;
                licenseReqs[id] = t;
                reqsToSpawnIds.push_back(id);
            }
        }

        for (int id : reqsToSpawnIds)
        {
            doPostLicense([this, id](){
                {
                    std::unique_lock lock(licenseMutex);
                    licenseReqs.erase(id);
                }
                callLicensePending();
            }, [this, id](){
                {
                    std::unique_lock lock(licenseMutex);
                    licenseReqs.erase(id);
                }
                spawnLicenseReqs();
            });
        }
    }

    void getLicenseId(const std::function<void (int)> &result)
    {
        {
            std::unique_lock lock(licenseMutex);
            licensePending.push_back(result);
        }

        callLicensePending();
    }

    void callLicensePending()
    {
        std::vector<std::function<void (int)>> calls;
        std::vector<int> ids;
        {
            std::unique_lock lock(licenseMutex);
            while (!licensePending.empty())
            {
                bool found = false;
                for (auto &p : licenses)
                {
                    if (p.second.remainingDigs > p.second.inFlyDigs)
                    {
                        p.second.inFlyDigs++;
                        calls.push_back(licensePending.front());
                        ids.push_back(p.first);
                        found = true;
                        licensePending.pop_front();
                        break;
                    }
                }

                if (!found)
                    break;
            }
        }

        for (size_t i = 0; i < ids.size(); ++i)
        {
            calls[i](ids[i]);
        }
    }

    void doPostLicense(const std::function<void ()> &result, const std::function<void ()>& onError)
    {
        postLicense([this, result](License r){
            {
                std::unique_lock lock(licenseMutex);
                LicenseUsage &u = licenses[r.id];
                u.inFlyDigs = 0;
                u.remainingDigs = r.digAllowed;
                licensePendingRequests--;
            }
            if (result)
                result();
        }, onError);
    }

    void licenseConsumed(int license)
    {
        bool postNew = false;
        {
            std::unique_lock lock(licenseMutex);
            auto it = licenses.find(license);
            assert(it != licenses.end());
            it->second.inFlyDigs--;
            it->second.remainingDigs--;
            if (it->second.remainingDigs == 0) {
                licenses.erase(it);
                licensePendingRequests++;
                postNew = true;
            }
        }

        if (postNew)
        {
            spawnLicenseReqs();
        }
    }


    // =========== EXPLORE =============
    void explore(KnownArea *a)
    {
        Area area{
                .pos = a->pos,
                .size = a->size
        };

        postExplore(area, [this, a](int amount){
            a->amount = amount;

            KnownArea *areasToDig[2];
            int areasToDigSize = 0;

            if (a->parent)
            {
                int siblingAmount = a->parent->amount - amount;
                KnownArea *sibling = a->parent->children[1];

                sibling->amount = siblingAmount;
                if (sibling->amount)
                {
                    if (sibling->isSingleCell()) {
                        areasToDig[areasToDigSize++] = sibling;
                    }
                    else {
                        areaMap.split(*sibling);
                    }
                }
            }

            if (a->amount > 0) {
                if (a->isSingleCell()) {
                    areasToDig[areasToDigSize++] = a;
                }
                else
                {
                    areaMap.split(*a);
                }
            }

            if (areasToDigSize == 0)
            {
                exploreRunOne();
            }
            else if (areasToDigSize == 1)
            {
                digArea(*areasToDig[0], std::bind(&State::exploreRunOne, this));
            }
            else
            {
                KnownArea * second = areasToDig[1];
                digArea(*areasToDig[0], [this, second]() {
                    digArea(*second, std::bind(&State::exploreRunOne, this));
                });
            }
        });
    }

    void exploreRunOne()
    {
        if (isRunning)
        {
            KnownArea *a = areaMap.nextArea();
            if (!a) {
                LOG("FIN");
                return;
            }

            try
            {
                explore(a);
            }
            catch (const std::runtime_error &err) {
            }
        }
    }
};

void printStats(State &state)
{
    double digCost = 0;
    double expCost = 0;
    double cashCost = 0;
    {
        std::lock_guard<std::mutex> g(timeStatsMutex);
        for (auto &p : _timeStats) {
            LOG("TT " << p.first << ": " << p.second);
            if (p.first.rfind("dig_", 0) == 0)
                digCost += p.second.cost;
            else if (p.first.rfind("exp_", 0) == 0)
                expCost += p.second.cost;
            else if (p.first.rfind("/cash200", 0) == 0)
                cashCost += p.second.count * 20.0;
        }
    }

    {
        std::lock_guard<std::mutex> g(cpuTimeStatsMutex);
        for (auto &p : _cpuTimeStats) {
            LOG("CT " << p.first << ": " << p.second);
        }
    }

    LOG("SCORE " << state.totalScore << " R " << totalReqs << " C " << foundCols << " DC " << digCost << " EC " << expCost << " T " << (digCost + expCost + cashCost));

    {
        std::lock_guard<std::mutex> g(thCostMutex);

        for (auto &p : thCost)
        {
            std::ostringstream oss;
            oss << p.first << ":";
            for (auto &p2 : p.second)
            {
                oss << "|" << p2.first << ":" << p2.second;
            }

            LOG("C" << oss.str());
        }
    }
}

int main() {
    LOG("E" << MAX_SMLT_EXPL_REQS << "_D" << MAX_SMLT_DIG_REQS << "_C" << MAX_SMLT_CASH_REQS << "_MC" << MIN_CASH_DEPTH << "_SW" << SPLIT_WID
    << "_WL" << WAIT_LICENSE_MS << "_TH" << THREADS << "_V1");

    TimeStatUpd upd;
    upd.start("total");
    State state;

    {
        CpuTimeMeasure t ("IO");
        auto work_guard = std::make_unique<work_guard_type>(ioc.get_executor());

        tcp::resolver resolver(ioc);

        resolveResults = resolver.resolve(getHost(), PORT);

        std::vector<std::thread> threads;

        for (int i = 0; i < THREADS; ++i) {
            threads.emplace_back([&]() {
                ioc.run();
            });
        }

        state.spawnLicenseReqs();

        for (int i = 0; i < MAX_SMLT_EXPL_REQS; ++i) {
            state.exploreRunOne();
        }

        while (isRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            state.spawnLicenseReqs();
        }

        work_guard.reset();

        for (auto &th : threads)
            th.join();
    }

    LOG("FINISH");

    upd.end();
    printStats(state);

    return 0;
}
