#include "opt_cuckoo.cpp"
#include <map>
int main()
{
    const int ops = 16000 * 1000;
    vector<pair<string, int>> args(ops);
    for (int i = 0; i < ops; i++)
    {
        args[i].first = "random:temp" + to_string(i);
        args[i].second = i;
    }
    for (int t_num = 1; t_num < 200; t_num += 1)
    {
        cout << "start" << endl;
        OptCuckoo<int> Cuckoo(8000 * 1000);
        vector<thread> threads;
        auto put = [](int tid, vector<pair<string, int>> &args, OptCuckoo<int> &Cuckoo, int t_num)
        {
            for (int i = tid; i < ops; i += t_num)
            {
                Cuckoo.put(args[i].first, args[i].second, tid + 1);
            }
        };

        auto s = get_now();
        for (int i = 0; i < t_num; i++)
        {
            threads.emplace_back(put, i, ref(args), ref(Cuckoo), t_num);
        }
        for (int i = 0; i < t_num; i++)
        {
            threads[i].join();
        }
        int abortsum=0;
        for(auto i:abort_tid)abortsum+=i;
        auto e = get_now();
        cout << "    threads : " << t_num << " time(ms) : " << get_duration_ms(s, e) / 1000000 << endl;
        cout << "    aborts : " << abortsum  << endl;
    }
}