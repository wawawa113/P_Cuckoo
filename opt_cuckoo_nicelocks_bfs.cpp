#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <cassert>
#include <unordered_map>
#include "common/hash.h"
#include "common/time.cpp"
#include <algorithm>
using namespace std;

// T val   string key
template <class T>
class OptCuckoo
{
public:
    class Data
    {
    public:
        string key;
        T val;
        Data()
        {
        }
        Data(string t_key, T t_val)
        {
            key = t_key;
            val = t_val;
        }
    };
    class Node
    {
    public:
        unsigned char tag; // why? how many bits?
        Data *data;
        Node()
        {
        }
        Node(unsigned char t_tag, string t_key, T t_val)
        {
            tag = t_tag;
            data = new Data(t_key, t_val);
        }
        // ~Node(){
        //     delete data;
        // }
    };
    // ~OptCuckoo(){
    //     for(auto &i:table){
    //         for(auto &j:i){
    //             if(j!=nullptr){
    //                 delete j;
    //                 j=nullptr;
    //             }
    //         }
    //     }
    // }

    const int SLOTS_NUM = 4;
    const int MAX_LOOP_FOR_PUT = 80 * 1000;

    int table_size;
    int key_size;
    int key_versions_size;

    vector<vector<Node *>> table;
    vector<vector<mutex>> table_locks;
    vector<vector<int>> visited;
    vector<vector<int>> key_versions;
    vector<vector<mutex>> key_versions_locks;

    // mutex giant_write_lock;

    unsigned char get_tag(const uint32_t input)
    {
        uint32_t t = input & 0xff;
        return (unsigned char)t + (t == 0);
    }

    void ABORT()
    {
        // this is for debug and for annotation
        // cout<<"ABORTED"<<endl;
    }

    OptCuckoo(int t_table_size)
    {
        table_size = t_table_size;
        table = vector<vector<Node *>>(table_size, vector<Node *>(SLOTS_NUM));
        visited = vector<vector<int>>(table_size, vector<int>(SLOTS_NUM));
        key_versions = vector<vector<int>>(table_size, vector<int>(SLOTS_NUM));
        for (int i = 0; i < table_size; i++)
        {
            key_versions_locks.emplace_back(vector<mutex>(SLOTS_NUM));
            table_locks.emplace_back(vector<mutex>(SLOTS_NUM));
        }

        key_versions_size = table_size;
        longest = vector<pair<pair<int, int>, T>>();
    }
    double get_version_t = 0;

    int get_version(int l, int r)
    {
        int res;
        auto s = get_now();
        key_versions_locks[l][r].lock();
        res = key_versions[l][r];
        key_versions_locks[l][r].unlock();
        auto e = get_now();
        get_version_t += get_duration_ms(s, e);
        return res;
    }
    double increase_version_t = 0;
    void increase_version(int l, int r)
    {
        auto s = get_now();
        key_versions_locks[l][r].lock();
        key_versions[l][r]++;
        key_versions_locks[l][r].unlock();
        auto e = get_now();
        increase_version_t += get_duration_ms(s, e);
    }

    T get(std::string key)
    {
        uint32_t h1 = 0, h2 = 0;
        hashlittle2(key.c_str(), key.length(), &h1, &h2);
        unsigned char tag = get_tag(h1);
        if (h1 < 0)
            h1 += table_size;
        if (h2 < 0)
            h2 += table_size;
        h1 = (h1) % table_size;
        h2 = (h2) % table_size;

        // let's try to get
        while (true)
        {
            for (int i = 0; i < SLOTS_NUM; i++)
            {
                Node *node = table[h1][i];
                uint32_t start_version = get_version(h1, i);
                if (node != NULL && node->tag == tag && node->data != NULL)
                {
                    Data *data = node->data;
                    if (key == data->key)
                    {
                        T val = data->val;
                        uint32_t end_version = get_version(h1, i);
                        if (start_version != end_version || start_version & 0x1)
                        {
                            ABORT();
                            continue;
                        }
                        return val;
                    }
                }
            }

            for (int i = 0; i < SLOTS_NUM; i++)
            {
                Node *node = table[h2][i];
                uint32_t start_version = get_version(h2, i);
                if (node != NULL && node->tag == tag && node->data != NULL)
                {
                    Data *data = node->data;
                    if (key == data->key)
                    {
                        T val = data->val;
                        uint32_t end_version = get_version(h2, i);
                        if (start_version != end_version || start_version & 0x1)
                        {
                            ABORT();
                            continue;
                        }
                        return val;
                    }
                }
            }

            // not found
            T temp_val;
            return temp_val;
        }
    }

    int aborted_num = 0;
    int validation_fail = 0;
    double sum_time = 0;
    int evict_null = 0;
    vector<pair<pair<int, int>, T>> longest;

    void put(std::string key, T val, int TID)
    {
        auto s = get_now();
        int i = 0;
        while (!put_impl(key, val, TID))
        {
            ABORT();
            i++;
            aborted_num++;
            if (i >= 100000)
            {
                cout << "Too many abort" << endl;
                exit(0);
                return;
            }
        }
        auto e = get_now();
        sum_time += get_duration_ms(s, e);
    }
    double write_lock_time = 0;
    double hash_time = 0;
    double w_1 = 0, w_2 = 0, w_3 = 0;
    int c1 = 0, c2 = 0, c3 = 0;

    bool put_impl(std::string key, T val, int TID)
    {
        string original_key = key;
        T original_val = val;
        unsigned char original_tag;

        vector<pair<uint32_t, int>> path;
        vector<int> path_versions_history;

        queue<pair<vector<pair<uint32_t, int>>, vector<int>>> que; //(path,version)
        bool found_shortest_path = false;

        uint32_t th1 = 0;
        uint32_t th2 = 0;
        hashlittle2(key.c_str(), key.length(), &th1, &th2);
        unsigned char tag = get_tag(th1);
        original_tag = tag;
        auto original_node = new Node(original_tag, original_key, original_val);
        if (th1 < 0)
            th1 += table_size;
        if (th2 < 0)
            th2 += table_size;
        th1 = (th1) % table_size;
        th2 = (th2) % table_size;

        // search in h1
        for (int i = 0; i < SLOTS_NUM; i++)
        {
            Node *node = table[th1][i];
            if (node == nullptr)
            {
                pair<int, int> index = make_pair(th1, i);
                table_locks[index.first][index.second].lock();
                if (table[index.first][index.second] != nullptr)
                {
                    ABORT();
                    table_locks[index.first][index.second].unlock();
                    continue;
                }
                increase_version(index.first, index.second);
                table[index.first][index.second] = original_node;
                increase_version(index.first, index.second);
                table_locks[index.first][index.second].unlock();
                return true;
            }
            else if (node->tag == tag && node->data->key == key)
            {
                exit(1);
                table_locks[th1][i].lock();
                if (node->data->key != key)
                {
                    table_locks[th1][i].unlock();
                    continue;
                }
                increase_version(th1, i);
                node->data->val = val;
                increase_version(th1, i);
                table_locks[th1][i].unlock();
                return true;
            }
        }
        // search in h2
        for (int i = 0; i < SLOTS_NUM; i++)
        {
            Node *node = table[th2][i];
            if (node == nullptr)
            {
                pair<int, int> index = make_pair(th2, i);
                table_locks[index.first][index.second].lock();
                if (table[index.first][index.second] != nullptr)
                {
                    ABORT();
                    table_locks[index.first][index.second].unlock();
                    continue;
                }
                increase_version(index.first, index.second);
                table[index.first][index.second] = original_node;
                increase_version(index.first, index.second);
                table_locks[index.first][index.second].unlock();
                return true;
            }
            else if (node->tag == tag && node->data->key == key)
            {
                exit(1);
                table_locks[th2][i].lock();
                if (node->data->key != key)
                {
                    table_locks[th2][i].unlock();
                    continue;
                }
                increase_version(th2, i);
                node->data->val = val;
                increase_version(th2, i);
                table_locks[th2][i].unlock();
                return true;
            }
        }

        // if both of them are full, do bfs
        for (int i = 0; i < SLOTS_NUM; i++)
        {
            {
                vector<pair<uint32_t, int>> tp;
                tp.push_back(make_pair(th1, i));
                vector<int> tv;
                tv.push_back(get_version(th1, i));
                que.push(make_pair(tp, tv));
            }
            {
                vector<pair<uint32_t, int>> tp;
                tp.push_back(make_pair(th2, i));
                vector<int> tv;
                tv.push_back(get_version(th2, i));
                que.push(make_pair(tp, tv));
            }
        }

        auto add_next_node = [&](vector<pair<uint32_t, int>> &p, vector<int> &v)
        {
            bool is_success = false;
            pair<int, int> before = p[p.size() - 1];
            assert(table[before.first][before.second] != NULL);
            string key = table[before.first][before.second]->data->key;
            T val = table[before.first][before.second]->data->val;
            uint32_t h1 = 0, h2 = 0;
            hashlittle2(key.c_str(), key.length(), &h1, &h2);
            unsigned char tag = get_tag(h1);
            if (h1 < 0)
                h1 += table_size;
            if (h2 < 0)
                h2 += table_size;
            h1 = (h1) % table_size;
            h2 = (h2) % table_size;

            // search in h1
            for (int i = 0; i < SLOTS_NUM; i++)
            {
                auto now = get_version(h1, i);
                Node *node = table[h1][i];
                if (node == NULL)
                {
                    v.push_back(now);
                    p.push_back(make_pair(h1, i));
                    is_success = true;
                    found_shortest_path = true;
                    break;
                }
            }
            if (is_success)
            {
                return;
            }

            // search in h2
            for (int i = 0; i < SLOTS_NUM; i++)
            {
                auto now = get_version(h2, i);
                Node *node = table[h2][i];
                if (node == NULL)
                {
                    v.push_back(now);
                    p.push_back(make_pair(h2, i));
                    is_success = true;
                    found_shortest_path = true;
                    break;
                }
            }
            if (is_success)
            {
                return;
            }

            // if both of them are full, choose node to evict
            Node *evict_node = NULL;
            for (int i = 0; i < SLOTS_NUM; i++)
            {
                if (find(p.begin(), p.end(), make_pair(h1, i)) == p.end())
                {
                    auto nv = v;
                    auto np = p;
                    nv.push_back(get_version(h1, i));
                    np.push_back(make_pair(h1, i));
                    que.push(make_pair(np, nv));
                }
                if (find(p.begin(), p.end(), make_pair(h2, i)) == p.end())
                {
                    auto nv = v;
                    auto np = p;
                    nv.push_back(get_version(h2, i));
                    np.push_back(make_pair(h2, i));
                    que.push(make_pair(np, nv));
                }
            }
        };
        int counter = 0;
        while (!que.empty())
        {
            auto now = que.front();
            que.pop();
            add_next_node(now.first, now.second);
            if (found_shortest_path)
            {
                path = now.first;
                path_versions_history = now.second;
                break;
            }
            counter++;
            // cout<<counter<<endl;
        }

        if (!found_shortest_path)
        {
            cout << "not found shortest path" << endl;

            ABORT();
            return false;
        }
        assert(path.size()!=1);
        // if (path.size() == 1)
        // {
        //     pair<int, int> index = path.front();
        //     table_locks[index.first][index.second].lock();
        //     if (table[index.first][index.second] != NULL)
        //     {
        //         ABORT();
        //         table_locks[index.first][index.second].unlock();
        //         return false;
        //     }
        //     increase_version(index.first, index.second);
        //     table[index.first][index.second] = original_node;
        //     increase_version(index.first, index.second);
        //     table_locks[index.first][index.second].unlock();
        //     return true;
        // }
        for (int i = path.size() - 1; i > 0; i--)
        {
            auto to = path[i];
            auto from = path[i - 1];

            assert((table[to.first][to.second]!=NULL || i==path.size()-1) && table[from.first][from.second]!=NULL);

            // lock smaller in first.
            if (to < from)
            {
                table_locks[to.first][to.second].lock();
                table_locks[from.first][from.second].lock();
            }
            else
            {
                table_locks[from.first][from.second].lock();
                table_locks[to.first][to.second].lock();
            }

            if (get_version(to.first, to.second) != path_versions_history[i] || path_versions_history[i] & 0x1)
            {
                ABORT();
                table_locks[to.first][to.second].unlock();
                table_locks[from.first][from.second].unlock();
                return false;
            }
            if (get_version(from.first, from.second) != path_versions_history[i - 1] || path_versions_history[i - 1] & 0x1)
            {
                ABORT();
                table_locks[to.first][to.second].unlock();
                table_locks[from.first][from.second].unlock();
                return false;
            }
            increase_version(to.first, to.second);
            table[to.first][to.second] = table[from.first][from.second];
            // table[from.first][from.second] = NULL;
            increase_version(to.first, to.second);
            table_locks[to.first][to.second].unlock();
            table_locks[from.first][from.second].unlock();
        }
        table_locks[path[0].first][path[0].second].lock();
        if (get_version(path[0].first, path[0].second) != path_versions_history[0] || path_versions_history[0] & 0x1)
        {
            ABORT();
            table_locks[path[0].first][path[0].second].unlock();
            return false;
        }
        increase_version(path[0].first, path[0].second);
        table[path[0].first][path[0].second] = original_node;
        increase_version(path[0].first, path[0].second);
        table_locks[path[0].first][path[0].second].unlock();
        return true;
    }
    void debug()
    {
        for (int i = 0; i < table_size; i++)
        {
            for (int j = 0; j < SLOTS_NUM; j++)
            {
                if (table[i][j] == NULL)
                {
                    cout << "VOID  NON   ";
                    continue;
                }
                cout << (int)table[i][j]->tag << " " << table[i][j]->data->key << "  " << table[i][j]->data->val << "   ";
            }
            cout << endl;
        }
    }
};