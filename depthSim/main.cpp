#include <iostream>
#include <set>

#define LOG(x)  std::cout << x << std::endl;

double digCost(int depth)
{
    //return 2.0 * (1.0 + (depth - 1) / 11.25);
    return 2.1 + 0.18*(depth - 1);
}

int main() {
    //int prices[] = {4, 9, 15, 19, 22, 24, 26, 28, 29, 30};
    //int prices[] = {4, 9, 15, 23, 27, 30, 33, 35, 37, 38};
    //int prices[] = {4, 9, 15, 23, 33, 37, 40, 43, 45, 47};
    //int prices[] = {4, 9, 15, 23, 33, 45, 49, 52, 55, 57};
    int prices[] = {4, 9, 15, 23, 33, 45, 60, 64, 67, 70};

    srand(time(0));
    for (int MIN_D = 1; MIN_D <= 6; ++MIN_D) {
        LOG("MIN_D " << MIN_D);
        //for (int MAX_D = 11; MAX_D-- > 1;)
        int MAX_D = 10;
        {
            double cost = 0;
            double dCost = 0;
            double eCost = 0;
            int coins = 0;
            int cols = 0;
            int cashes = 0;
            while (cost < 1200000) {
                cols++;
                cost += 8.3;
                eCost += 8.3;
                int d = rand() % 10 + 1;

                for (int i = 1; i <= d && i <= MAX_D; ++i) {
                    cost += digCost(i);
                    dCost += digCost(i);
                }

                if (d >= MIN_D && d <= MAX_D) {
                    coins += prices[d - 1];
                    cost += 20;
                    cashes++;
                }
            }

            std::cout << MAX_D << " " << coins << " C " << cols << " CSH " << cashes << " E " << eCost << " D " << dCost << std::endl;
        }
    }
    return 0;
}