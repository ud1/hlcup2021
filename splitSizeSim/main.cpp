#include <iostream>
#include <map>
#include <fstream>
#include <vector>
#include <set>

#define LOG(x)  std::cout << x << std::endl;

int exploreCost(int area)
{
    if (area <= 3)
        return 1;

    int res = 1;
    for (int i = 4; i <= area; i *= 2)
    {
        res += 1;
    }

    return res;
}

/*int rndAmt(double amt)
{
    int res = amt * AMT_MULT + 0.5;
    if (res == 0 && amt > 0)
        return 1;

    return res;
}*/

double prob(double amt)
{
    return std::min(amt, 1.0);
}

std::pair<int, double> split(int a) {
    if (a <= 1)
        return std::make_pair(0, 0);

    if (a == 2)
        return std::make_pair(1, 1);

    static std::map<int, std::pair<int, double>> cache;
    if (cache.count(a))
        return cache[a];


    int bestS = 0;
    double bestRes = 0;
    for (int i = 1; i <= a / 2; ++i)
    {
        double amt1 = std::min(1.0, i / (double) a);
        double amt2 = std::min(1.0, (a - i) / (double) a);

        double res = split(i).second * amt1 + split(a - i).second * amt2 + exploreCost(i);

        if (bestS == 0 || bestRes > res)
        {
            bestRes = res;
            bestS = i;
        }
    }

    cache[a] = std::make_pair(bestS, bestRes);
    //std::cout << a << " " << amt << " " << bestS << " " << bestRes << std::endl;
    return std::make_pair(bestS, bestRes);
}

std::string charMap = "0123456789abcdefghijklnopqrstuvwxyzABCDEFGHIJKLNOPQRSTUVWXYZ!+-*/|";

void decode(std::map<int, std::map<int, int>> &out, std::string line)
{
    int i = line.find_first_of(":");
    int y = atoi(line.substr(0, i).c_str());
    std::string r = line.substr(i + 1);

    std::map<char, int> inverseCharMap;
    for (int i = 0; i < charMap.length(); ++i)
        inverseCharMap[charMap[i]] = i;

    int dx = 0;
    for (int i = 0; i < r.size(); i += 4)
    {
        std::string s = r.substr(i, i + 4);

        int am = inverseCharMap[s[0]];

        std::vector<std::pair<int, int>> tri;
        tri.resize(3);

        for (int i = 3; i --> 0;)
        {
            tri[i].second = am % 4;
            am /= 4;
        }

        for (int i = 0; i < 3; ++i)
        {
            tri[i].first = inverseCharMap[s[i + 1]];
        }


        for (int i = 0; i < 3; ++i)
        {
            dx += tri[i].first;
            if (tri[i].second > 0)
            {
                out[y][dx] = tri[i].second;
            }
            dx++;
        }
    }
}

int totalCost = 0;
int foundCols = 0;

std::set<std::pair<int, int>> points;

int count(std::pair<int, int> p, int sz)
{
    int cnt = 0;
    for (int i = 0; i < sz; ++i) {
        if (points.count(std::make_pair(p.first + i, p.second)))
            ++cnt;
    }

    totalCost += exploreCost(sz);
    if (totalCost > 250000)
    {
        LOG("COLS " << foundCols << " " << ((double) totalCost / (double) foundCols));
        exit(0);
    }

    return cnt;
}

// COLS 29503 8.47375
// COLS 29504 8.47346
// COLS 29515 8.4703
// COLS 29516 8.47002
// COLS 29517 8.46983
// COLS 29571 8.45426
// COLS 29578 8.45226
// COLS 29582 8.45112
int splitSize(int size, int thCount)
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

const int SPLIT_WID = 255;

int process(int x, int y, int s, int c = -1)
{
    const int cnt = c >= 0 ? c : count(std::make_pair(x, y), s);

    if (cnt > 0) {
        if (s == 1)
        {
            foundCols++;
            return cnt;
        }

        int splitS = splitSize(s, cnt);
        if (splitS == 0)
            return cnt;

        int firstA = process(x, y, splitS);
        int vB = cnt - firstA;

        if (vB > 0)
        {
            double dB = vB / (double) (s - splitS);
            process(x + splitS, y, s - splitS, vB);
        }
    }

    return cnt;
}

void loadDigpointsFromFile()
{
    std::map<int, std::map<int, int>> digPoints;

    std::ifstream file("coords");

    std::string line;

    while (file >> line)
    {
        if (line.size() > 0)
        {
            decode(digPoints, line);
        }
    }

    for (auto &p : digPoints)
    {
        for (auto &p2 : p.second)
        {
            int y = p.first;
            int x = p2.first;
            points.insert(std::make_pair(x, y));
        }
    }



    std::map<int, int> cntMap;
    int totalPoints = 0;
    int totalAreas = 0;

    for (int y = 0; y < 3000; ++y)
    {
        for (int x = 0; x < 1400; x += SPLIT_WID)
        {
            int cnt = 0;

            for (int i = 0; i < SPLIT_WID; ++i) {
                if (points.count(std::make_pair(x + i, y)))
                    ++cnt;
            }
            cntMap[cnt]++;
            totalPoints += cnt;
            totalAreas++;

            process(x, y, SPLIT_WID);
            //std::cout << " " << cnt;
        }
        //std::cout << std::endl;
    }

    double tot = 0;
    for (auto &p : cntMap)
    {
        tot += p.second;
        LOG("C " << p.first << ", " << p.second << " " << (tot / totalAreas));
    }

    LOG("TP " << totalPoints << " TA " << totalAreas << " R " << ((double) totalPoints / (double) totalAreas));
}

int main() {
    /*std::cout << split(2, 2.0/8).second << std::endl;

    std::cout << split(3, 3.0/8).second << std::endl;
    std::cout << split(5, 5.0/8).second << std::endl;*/
    /*for (int i = 1; i <= 256; ++i) {
        std::pair<int, double> v = split(i);
        std::cout << i << ": " << v.first << " - " << v.second  << std::endl;
    }*/

    if (true)
    {
        loadDigpointsFromFile();
        return 0;
    }

    int s = 0;
    int d = 10;
    for (int i = 0; i < 1000; ++i)
    {
        //int l = std::max(rand() % d, rand() % d) + 1;
        int l = rand() % d + 1;
        if (l == d)
            l = d - 1;
        s += l;
    }

    std::cout << (s / 1000.0) << std::endl;


    return 0;
}