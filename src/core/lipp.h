#ifndef __LIPP_H__
#define __LIPP_H__

#include "lipp_base.h"
#include <stdint.h>
#include <math.h>
#include <limits>
#include <iostream>
#include <cstdio>
#include <stack>
#include <vector>
#include <cstring>
#include <sstream>

typedef uint8_t bitmap_t;

#define BITMAP_WIDTH (sizeof(bitmap_t) * 8)
#define BITMAP_SIZE(num_items) (((num_items) + BITMAP_WIDTH - 1) / BITMAP_WIDTH)
#define BITMAP_GET(bitmap, pos) (((bitmap)[(pos) / BITMAP_WIDTH] >> ((pos) % BITMAP_WIDTH)) & 1)
#define BITMAP_SET(bitmap, pos) ((bitmap)[(pos) / BITMAP_WIDTH] |= 1 << ((pos) % BITMAP_WIDTH))
#define BITMAP_CLEAR(bitmap, pos) ((bitmap)[(pos) / BITMAP_WIDTH] &= ~bitmap_t(1 << ((pos) % BITMAP_WIDTH)))
#define BITMAP_NEXT_1(bitmap_item) __builtin_ctz((bitmap_item))

// runtime assert
#define RT_ASSERT(expr)                                                                     \
    {                                                                                       \
        if (!(expr))                                                                        \
        {                                                                                   \
            fprintf(stderr, "RT_ASSERT Error at %s:%d, `%s`\n", __FILE__, __LINE__, #expr); \
            exit(0);                                                                        \
        }                                                                                   \
    }

#define COLLECT_TIME 0

#if COLLECT_TIME
#include <chrono>
#endif

template <class T, class P, bool USE_FMCD = true>
class LIPP {
    struct Node;

    static_assert(std::is_arithmetic<T>::value, "LIPP key type must be numeric.");

    inline int compute_gap_count(int size) {
        for (auto &p : gap_counts) {
            if (p.first < 0 || size >= p.first) {
                return p.second;
            }
        }
    }

    // inline int compute_gap_count(int size)
    // {
    //     if (size >= 1000000)
    //         return 1;
    //     else if (size >= 100000)
    //         return 2;
    //     else
    //         return 5;
    // }

    inline int PREDICT_POS(Node *node, T key) const {
        double v = node->model.predict_double(key);
        if (v > std::numeric_limits<int>::max() / 2)
        {
            return node->num_items - 1;
        }
        if (v < 0)
        {
            return 0;
        }
        return std::min(node->num_items - 1, static_cast<int>(v));
    }

    static void remove_last_bit(bitmap_t &bitmap_item) {
        bitmap_item -= 1 << BITMAP_NEXT_1(bitmap_item);
    }

    const double BUILD_LR_REMAIN;
    const bool QUIET;

    struct {
        long long fmcd_success_times = 0;
        long long fmcd_broken_times = 0;
#if COLLECT_TIME
        double time_scan_and_destory_tree = 0;
        double time_build_tree_bulk = 0;
#endif
    } stats;

public:
    typedef std::pair<T, P> V;

    int num_rebuild = 0;
    int default_num_items;
    std::vector<std::pair<int, int>> gap_counts{{1000000, 1}, {100000, 2}, {-1, 5}};

    LIPP(double BUILD_LR_REMAIN = 0, bool QUIET = true) : BUILD_LR_REMAIN(BUILD_LR_REMAIN), QUIET(QUIET) {
    }

    ~LIPP() {
        destroy_tree(root);
        root = NULL;
        destory_pending();
    }

    void init() {
        {
            std::vector<Node *> nodes;
            for (int _ = 0; _ < 1e7; _++) {
                Node *node = build_tree_two(T(0), P(), T(1), P());
                nodes.push_back(node);
            }
            for (auto node : nodes) {
                destroy_tree(node);
            }
            if (!QUIET) {
                printf("Initial memory pool size = %lu\n", pending_two.size());
            }
        }

        if (USE_FMCD && !QUIET) {
            printf("Enable FMCD\n");
        }

        root = build_tree_none();
    }

    bool insert(const V &v) {
        return insert(v.first, v.second);
    }

    bool insert(const T &key, const P &value) {
        bool ok = true;
        root = insert_tree(root, key, value, &ok);
        return ok;
    }

    P at(const T &key, bool skip_existence_check, bool &exist) const {
        Node *node = root;
        exist = true;

        while (true) {
            int pos = PREDICT_POS(node, key);

            if (BITMAP_GET(node->child_bitmap, pos) == 1) {
                node = node->items[pos].comp.child;
            }
            else {
                if (skip_existence_check) {
                    return node->items[pos].comp.data.value;
                }
                else {
                    if (BITMAP_GET(node->none_bitmap, pos) == 1) {
                        exist = false;
                        return static_cast<P>(0);
                    }
                    else if (BITMAP_GET(node->child_bitmap, pos) == 0) {
                        // RT_ASSERT(node->items[pos].comp.data.key == key);
                        return node->items[pos].comp.data.value;
                    }
                }
            }
        }
    }

    bool exists(const T &key) const {
        Node *node = root;

        while (true) {
            int pos = PREDICT_POS(node, key);
            if (BITMAP_GET(node->none_bitmap, pos) == 1) {
                return false;
            }
            else if (BITMAP_GET(node->child_bitmap, pos) == 0) {
                return node->items[pos].comp.data.key == key;
            }
            else {
                node = node->items[pos].comp.child;
            }
        }
    }

    void bulk_load(const V *vs, int num_keys) {
        if (num_keys == 0) {
            destroy_tree(root);
            root = build_tree_none();
            return;
        }
        if (num_keys == 1) {
            destroy_tree(root);
            root = build_tree_none();
            insert(vs[0]);
            return;
        }
        if (num_keys == 2) {
            destroy_tree(root);
            root = build_tree_two(vs[0].first, vs[0].second, vs[1].first, vs[1].second);
            return;
        }

        RT_ASSERT(num_keys > 2);
        for (int i = 1; i < num_keys; i++) {
            RT_ASSERT(vs[i].first > vs[i - 1].first);
        }

        T *keys = new T[num_keys];
        P *values = new P[num_keys];

        for (int i = 0; i < num_keys; i++) {
            keys[i] = vs[i].first;
            values[i] = vs[i].second;
        }

        destroy_tree(root);
        root = build_tree_bulk(keys, values, num_keys);

        delete[] keys;
        delete[] values;
    }

    void bulk_load(const V *vs, int num_keys, const std::string &dataset) {
        this->bulk_load(vs, num_keys);
        this->print_distribution(dataset);
    }

    void print(const std::string &dataset) {
        this->print_distribution(dataset);
    }

    bool remove(const T &key) {
        constexpr int MAX_DEPTH = 128;
        int path_size = 0;
        Node *path[MAX_DEPTH];
        Node *parent = nullptr;

        for (Node *node = root;;) {
            RT_ASSERT(path_size < MAX_DEPTH);
            path[path_size++] = node;
            // node->size--;
            int pos = PREDICT_POS(node, key);

            if (BITMAP_GET(node->child_bitmap, pos) == 1) {
                parent = node;
                node = node->items[pos].comp.child;
            }
            else if (BITMAP_GET(node->none_bitmap, pos) == 1) {
                return false;
            }
            else if (BITMAP_GET(node->child_bitmap, pos) == 0) {
                BITMAP_SET(node->none_bitmap, pos);
                for (int i = 0; i < path_size; i++) {
                    path[i]->size--;
                }
                if (node->size == 0) {
                    int parent_pos = PREDICT_POS(parent, key);
                    BITMAP_CLEAR(parent->child_bitmap, parent_pos);
                    BITMAP_SET(parent->none_bitmap, parent_pos);
                    delete_items(node->items, node->num_items);
                    const int bitmap_size = BITMAP_SIZE(node->num_items);
                    delete_bitmap(node->none_bitmap, bitmap_size);
                    delete_bitmap(node->child_bitmap, bitmap_size);
                    delete_nodes(node, 1);
                }
                return true;
            }
        }
    }

    bool update(const T &key, const P &value) {
        for (Node *node = root;;) {
            int pos = PREDICT_POS(node, key);
            if (BITMAP_GET(node->none_bitmap, pos) == 1) {
                return false;
            }
            else if (BITMAP_GET(node->child_bitmap, pos) == 0) {
                node->items[pos].comp.data.value = value;
                return true;
            }
            else {
                node = node->items[pos].comp.child;
            }
        }
    }

    // Find the minimum `len` keys which are no less than `lower`, returns the number of found keys.
    int range_query_len(std::pair<T, P> *results, const T &lower, int len, const std::string &dataset) {
        if (dataset == "") {
            return 0;
        }

        // std::stringstream filename;
        // filename << dataset << "_count" << ".csv";

        // std::string output_file = filename.str();
        // std::cout << "output_file = " << output_file << std::endl;

        // std::ifstream ifs(filename.str());
        // bool is_file_exist = ifs.good();
        // ifs.close();

        // std::ofstream null_ofs("null.csv");
        // std::ofstream ofs(output_file, std::ios::app);

        // if (!is_file_exist) {
        //     std::cout << "create new file!" << std::endl;
        //     ofs << "depth,up,down" << std::endl;
        // }

        // Node *node = this->find_node(lower);

        int max_depth = 0;
        int result = this->range_core_len<false>(results, 0, root, lower, len);
        // this->print_distribution_recursive(node, null_ofs, 0, max_depth);

        // for (auto &pair : this->count_map) {
        //     int up = pair.second.up;
        //     int down = pair.second.down;

        //     if (up > 0 && down > 0) {
        //         int depth = this->print_depth(pair.first);
        //         ofs << depth << "," << up << "," << down << std::endl;
        //     }
        // }

        // ofs << "--------------------------" << std::endl;
        // this->count_map.clear();

        return result;
    }

    void show() const {
        std::stack<Node *> s;
        s.push(root);

        printf("============= SHOW LIPP =============\n");

        while (!s.empty()) {
            Node *node = s.top();
            s.pop();

            printf("Node(%p, a = %lf, b = %lf, num_items = %d)", node, node->model.a, node->model.b, node->num_items);
            printf("[");

            int first = 1;

            for (int i = 0; i < node->num_items; i++) {
                if (!first) {
                    printf(", ");
                }
                first = 0;
                if (BITMAP_GET(node->none_bitmap, i) == 1) {
                    printf("None");
                }
                else if (BITMAP_GET(node->child_bitmap, i) == 0) {
                    std::stringstream s;
                    s << node->items[i].comp.data.key;
                    printf("Key(%s)", s.str().c_str());
                }
                else {
                    printf("Child(%p)", node->items[i].comp.child);
                    s.push(node->items[i].comp.child);
                }
            }
            printf("]\n");
        }
    }

    int print_depth(Node *to) const {
        int max_depth = 1, sum_depth = 0, sum_nodes = 0, depth = 0;
        std::stack<Node *> s;
        std::stack<int> d;

        s.push(root);
        d.push(0);

        while (!s.empty()) {
            Node *node = s.top();
            depth = d.top();

            s.pop();
            d.pop();

            if (node && node == to) {
                return depth;
            }

            for (int i = 0; i < node->num_items; i++) {
                if (BITMAP_GET(node->child_bitmap, i) == 1) {
                    s.push(node->items[i].comp.child);
                    d.push(depth + 1);
                }
                else if (BITMAP_GET(node->none_bitmap, i) != 1) {
                    max_depth = std::max(max_depth, depth);
                    sum_depth += depth;
                    sum_nodes++;
                }
            }
        }

        printf("max_depth = %d, avg_depth = %.2lf\n", max_depth, double(sum_depth) / double(sum_nodes));
        return max_depth;
    }

    void verify() const {
        std::stack<Node *> s;
        s.push(root);

        while (!s.empty()) {
            Node *node = s.top();
            int sum_size = 0;

            s.pop();

            for (int i = 0; i < node->num_items; i++) {
                if (BITMAP_GET(node->child_bitmap, i) == 1) {
                    s.push(node->items[i].comp.child);
                    sum_size += node->items[i].comp.child->size;
                }
                else if (BITMAP_GET(node->none_bitmap, i) != 1) {
                    sum_size++;
                }
            }
            RT_ASSERT(sum_size == node->size);
        }
    }

    void print_stats() const {
        printf("======== Stats ===========\n");
        if (USE_FMCD) {
            printf("\t fmcd_success_times = %lld\n", stats.fmcd_success_times);
            printf("\t fmcd_broken_times = %lld\n", stats.fmcd_broken_times);
        }
#if COLLECT_TIME
        printf("\t time_scan_and_destory_tree = %lf\n", stats.time_scan_and_destory_tree);
        printf("\t time_build_tree_bulk = %lf\n", stats.time_build_tree_bulk);
#endif
    }

    size_t index_size() const {
        size_t size = 0;
        std::stack<Node *> s;

        s.push(root);

        while (!s.empty()) {
            Node *node = s.top();
            s.pop();

            size += sizeof(*node);
            size += sizeof(*(node->none_bitmap));
            size += sizeof(*(node->child_bitmap));

            for (int i = 0; i < node->num_items; i++) {
                if (BITMAP_GET(node->child_bitmap, i) == 1) {
                    s.push(node->items[i].comp.child);
                    size += sizeof(Item);
                }
                else {
                    if (BITMAP_GET(node->none_bitmap, i) == 1) {
                        size += sizeof(Item);
                    }
                }
            }
        }
        return size;
    }

    size_t total_size() const {
        size_t size = 0;
        std::stack<Node *> s;

        s.push(root);

        while (!s.empty()) {
            Node *node = s.top();
            s.pop();

            size += sizeof(*node);
            size += sizeof(*(node->none_bitmap));
            size += sizeof(*(node->child_bitmap));

            for (int i = 0; i < node->num_items; i++) {
                size += sizeof(Item);
                if (BITMAP_GET(node->child_bitmap, i) == 1) {
                    s.push(node->items[i].comp.child);
                }
            }
        }
        return size;
    }

private:
    struct Node;

    struct Item {
        union {
            struct {
                T key;
                P value;
            } data;
            Node *child;
        } comp;
    };

    struct Node {
        int is_two;     // is special node for only two keys
        int build_size; // tree size (include sub nodes) when node created
        int size;       // current tree size (include sub nodes)
        int fixed;      // fixed node will not trigger rebuild
        int num_inserts, num_insert_to_data;
        int num_items; // size of items
        LinearModel<T> model;
        Item *items;
        bitmap_t *none_bitmap;  // 1 means None, 0 means Data or Child
        bitmap_t *child_bitmap; // 1 means Child. will always be 0 when none_bitmap is 1

        size_t memory_size() const {
            size_t size = sizeof(*this);
            size += sizeof(*none_bitmap) * BITMAP_SIZE(num_items);
            size += sizeof(*child_bitmap) * BITMAP_SIZE(num_items);
            size += sizeof(Item) * num_items;
            return size;
        }
    };

    struct Stat {
        unsigned int total;
        unsigned int null;
        unsigned int data;
        unsigned int node;
        unsigned int depth;
    };

    struct Count {
        unsigned int up = 0;
        unsigned int down = 0;
    };

    Node *root;
    std::stack<Node *> pending_two;
    std::allocator<Node> node_allocator;
    std::allocator<Item> item_allocator;
    std::allocator<bitmap_t> bitmap_allocator;
    std::unordered_map<Node *, Count> count_map;

    Node *new_nodes(int n) {
        Node *p = node_allocator.allocate(n);
        RT_ASSERT(p != NULL && p != (Node *)(-1));
        return p;
    }

    void delete_nodes(Node *p, int n) {
        node_allocator.deallocate(p, n);
    }

    Item *new_items(int n) {
        Item *p = item_allocator.allocate(n);
        RT_ASSERT(p != NULL && p != (Item *)(-1));
        return p;
    }

    void delete_items(Item *p, int n) {
        item_allocator.deallocate(p, n);
    }

    bitmap_t *new_bitmap(int n) {
        bitmap_t *p = bitmap_allocator.allocate(n);
        RT_ASSERT(p != NULL && p != (bitmap_t *)(-1));
        return p;
    }

    void delete_bitmap(bitmap_t *p, int n) {
        bitmap_allocator.deallocate(p, n);
    }

    Node *find_node(const T &key) {
        Node *node = root;

        while (true) {
            int pos = PREDICT_POS(node, key);
            if (BITMAP_GET(node->child_bitmap, pos) == 1) {
                node = node->items[pos].comp.child;
            }
            else {
                if (BITMAP_GET(node->none_bitmap, pos) == 1) {
                    return nullptr;
                }
                return node;
            }
        }
    }

    void print_distribution_recursive(Node *node, std::ofstream &ofs, int current_depth, int &max_depth) {
        if (node == nullptr) {
            return;
        }

        Stat stat{node->num_items, 0, 0, 0, current_depth};
        
        max_depth = std::max(current_depth, max_depth);

        for (int i = 0; i < node->num_items; i++) {
            if (BITMAP_GET(node->none_bitmap, i) == 1) {
                stat.null++;
                continue;
            }
            else if (BITMAP_GET(node->child_bitmap, i) == 0) {
                stat.data++;
            }
            else {
                stat.node++;
                print_distribution_recursive(node->items[i].comp.child, ofs, current_depth + 1, max_depth);
            }
        }

        if (!ofs.fail()) {
            ofs << stat.total << "," << stat.null << "," << stat.data << "," << stat.node << "," << stat.depth << std::endl;
        }
    }

    void print_distribution(const std::string &dataset)
    {
        std::stringstream filename;
        if (default_num_items > 0) {
            if (gap_counts.size() > 0) {
                auto pair = gap_counts.back();
                filename << dataset << "___" << default_num_items << "___" << pair.second << ".csv";
            }
            else {
                filename << dataset << "___" << default_num_items << ".csv";
            }
        }
        else {
            filename << dataset << ".csv";
        }

        std::string output_file = filename.str();
        std::cout << "output_file = " << output_file << std::endl;

        std::ofstream ofs(output_file);
        ofs << "total,null,data,node,depth\n";

        int max_depth = 0;

        print_distribution_recursive(this->root, ofs, 0, max_depth);

        std::cout << std::endl;
        ofs.close();
    }

    /// build an empty tree
    Node *build_tree_none() {
        Node *node = new_nodes(1);
        node->is_two = 0;
        node->build_size = 0;
        node->size = 0;
        node->fixed = 0;
        node->num_inserts = node->num_insert_to_data = 0;
        node->num_items = 1;
        node->model.a = node->model.b = 0;
        node->items = new_items(1);
        node->none_bitmap = new_bitmap(1);
        node->none_bitmap[0] = 0;
        BITMAP_SET(node->none_bitmap, 0);
        node->child_bitmap = new_bitmap(1);
        node->child_bitmap[0] = 0;
        return node;
    }

    /// build a tree with two keys
    Node *build_tree_two(T key1, P value1, T key2, P value2) {
        if (key1 > key2) {
            std::swap(key1, key2);
            std::swap(value1, value2);
        }

        RT_ASSERT(key1 < key2);
        static_assert(BITMAP_WIDTH == 8);

        Node *node = NULL;

        if (pending_two.empty()) {
            node = new_nodes(1);
            node->is_two = 1;
            node->build_size = 2;
            node->size = 2;
            node->fixed = 0;
            node->num_inserts = node->num_insert_to_data = 0;
            node->num_items = default_num_items;
            node->items = new_items(node->num_items);
            node->none_bitmap = new_bitmap(1);
            node->child_bitmap = new_bitmap(1);
            node->none_bitmap[0] = 0xff;
            node->child_bitmap[0] = 0;
        }
        else {
            node = pending_two.top();
            pending_two.pop();
        }

        const long double mid1_key = key1;
        const long double mid2_key = key2;

        const double mid1_target = node->num_items / 3;
        const double mid2_target = node->num_items * 2 / 3;

        node->model.a = (mid2_target - mid1_target) / (mid2_key - mid1_key);
        node->model.b = mid1_target - node->model.a * mid1_key;

        RT_ASSERT(isfinite(node->model.a));
        RT_ASSERT(isfinite(node->model.b));

        { // insert key1 & value1
            int pos = PREDICT_POS(node, key1);
            // RT_ASSERT(BITMAP_GET(node->none_bitmap, pos) == 1);
            BITMAP_CLEAR(node->none_bitmap, pos);
            node->items[pos].comp.data.key = key1;
            node->items[pos].comp.data.value = value1;
        }
        { // insert key2 & value2
            int pos = PREDICT_POS(node, key2);
            // RT_ASSERT(BITMAP_GET(node->none_bitmap, pos) == 1);
            BITMAP_CLEAR(node->none_bitmap, pos);
            node->items[pos].comp.data.key = key2;
            node->items[pos].comp.data.value = value2;
        }

        return node;
    }

    /// bulk build, _keys must be sorted in asc order.
    Node *build_tree_bulk(T *_keys, P *_values, int _size) {
        if (USE_FMCD) {
            return build_tree_bulk_fmcd(_keys, _values, _size);
        }
        else {
            return build_tree_bulk_fast(_keys, _values, _size);
        }
    }

    /// bulk build, _keys must be sorted in asc order.
    /// split keys into three parts at each node.
    Node *build_tree_bulk_fast(T *_keys, P *_values, int _size) {
        RT_ASSERT(_size > 1);

        typedef struct {
            int begin;
            int end;
            int level; // top level = 1
            Node *node;
        } Segment;
        std::stack<Segment> s;

        Node *ret = new_nodes(1);
        s.push((Segment){0, _size, 1, ret});

        while (!s.empty()) {
            const int begin = s.top().begin;
            const int end = s.top().end;
            const int level = s.top().level;
            Node *node = s.top().node;

            s.pop();

            RT_ASSERT(end - begin >= 2);

            if (end - begin == 2) {
                Node *_ = build_tree_two(_keys[begin], _values[begin], _keys[begin + 1], _values[begin + 1]);
                memcpy(node, _, sizeof(Node));
                delete_nodes(_, 1);
            }
            else {
                T *keys = _keys + begin;
                P *values = _values + begin;
                const int size = end - begin;
                const int BUILD_GAP_CNT = compute_gap_count(size);

                node->is_two = 0;
                node->build_size = size;
                node->size = size;
                node->fixed = 0;
                node->num_inserts = node->num_insert_to_data = 0;

                int mid1_pos = (size - 1) / 3;
                int mid2_pos = (size - 1) * 2 / 3;

                RT_ASSERT(0 <= mid1_pos);
                RT_ASSERT(mid1_pos < mid2_pos);
                RT_ASSERT(mid2_pos < size - 1);

                const long double mid1_key = (static_cast<long double>(keys[mid1_pos]) + static_cast<long double>(keys[mid1_pos + 1])) / 2;
                const long double mid2_key = (static_cast<long double>(keys[mid2_pos]) + static_cast<long double>(keys[mid2_pos + 1])) / 2;

                node->num_items = size * static_cast<int>(BUILD_GAP_CNT + 1);
                const double mid1_target = mid1_pos * static_cast<int>(BUILD_GAP_CNT + 1) + static_cast<int>(BUILD_GAP_CNT + 1) / 2;
                const double mid2_target = mid2_pos * static_cast<int>(BUILD_GAP_CNT + 1) + static_cast<int>(BUILD_GAP_CNT + 1) / 2;

                node->model.a = (mid2_target - mid1_target) / (mid2_key - mid1_key);
                node->model.b = mid1_target - node->model.a * mid1_key;

                RT_ASSERT(isfinite(node->model.a));
                RT_ASSERT(isfinite(node->model.b));

                const int lr_remains = static_cast<int>(size * BUILD_LR_REMAIN);
                node->model.b += lr_remains;
                node->num_items += lr_remains * 2;

                if (size > 1e6) {
                    node->fixed = 1;
                }

                node->items = new_items(node->num_items);
                const int bitmap_size = BITMAP_SIZE(node->num_items);
                node->none_bitmap = new_bitmap(bitmap_size);
                node->child_bitmap = new_bitmap(bitmap_size);
                memset(node->none_bitmap, 0xff, sizeof(bitmap_t) * bitmap_size);
                memset(node->child_bitmap, 0, sizeof(bitmap_t) * bitmap_size);

                for (int item_i = PREDICT_POS(node, keys[0]), offset = 0; offset < size;) {
                    int next = offset + 1, next_i = -1;

                    while (next < size) {
                        next_i = PREDICT_POS(node, keys[next]);
                        if (next_i == item_i) {
                            next++;
                        }
                        else {
                            break;
                        }
                    }

                    if (next == offset + 1) {
                        BITMAP_CLEAR(node->none_bitmap, item_i);
                        node->items[item_i].comp.data.key = keys[offset];
                        node->items[item_i].comp.data.value = values[offset];
                    }
                    else {
                        // ASSERT(next - offset <= (size+2) / 3);
                        BITMAP_CLEAR(node->none_bitmap, item_i);
                        BITMAP_SET(node->child_bitmap, item_i);
                        node->items[item_i].comp.child = new_nodes(1);
                        s.push((Segment){begin + offset, begin + next, level + 1, node->items[item_i].comp.child});
                    }

                    if (next >= size) {
                        break;
                    }
                    else {
                        item_i = next_i;
                        offset = next;
                    }
                }
            }
        }

        return ret;
    }

    /// bulk build, _keys must be sorted in asc order.
    /// FMCD method.
    Node *build_tree_bulk_fmcd(T *_keys, P *_values, int _size) {
        RT_ASSERT(_size > 1);

        typedef struct {
            int begin;
            int end;
            int level; // top level = 1
            Node *node;
        } Segment;
        std::stack<Segment> s;

        Node *ret = new_nodes(1);
        s.push((Segment){0, _size, 1, ret});

        while (!s.empty()) {
            const int begin = s.top().begin;
            const int end = s.top().end;
            const int level = s.top().level;
            Node *node = s.top().node;

            s.pop();

            RT_ASSERT(end - begin >= 2);

            if (end - begin == 2) {
                Node *_ = build_tree_two(_keys[begin], _values[begin], _keys[begin + 1], _values[begin + 1]);
                memcpy(node, _, sizeof(Node));
                delete_nodes(_, 1);
            }
            else {
                T *keys = _keys + begin;
                P *values = _values + begin;
                const int size = end - begin;
                const int BUILD_GAP_CNT = compute_gap_count(size);

                node->is_two = 0;
                node->build_size = size;
                node->size = size;
                node->fixed = 0;
                node->num_inserts = node->num_insert_to_data = 0;

                // FMCD method
                // Here the implementation is a little different with Algorithm 1 in our paper.
                // In Algorithm 1, U_T should be (keys[size - 1 - D] - keys[D]) / (L - 2).
                // But according to the derivation described in our paper, M.A should be less than 1 / U_T.
                // So we added a small number (1e-6) to U_T.
                // In fact, it has only a negligible impact of the performance.
                {
                    const int L = size * static_cast<int>(BUILD_GAP_CNT + 1);
                    int i = 0;
                    int D = 1;

                    RT_ASSERT(D <= size - 1 - D);

                    double Ut = (static_cast<long double>(keys[size - 1 - D]) - static_cast<long double>(keys[D])) / (static_cast<double>(L - 2)) + 1e-6;

                    while (i < size - 1 - D) {
                        while (i + D < size && keys[i + D] - keys[i] >= Ut) {
                            i++;
                        }

                        if (i + D >= size) {
                            break;
                        }

                        D = D + 1;
                        if (D * 3 > size) {
                            break;
                        }

                        RT_ASSERT(D <= size - 1 - D);

                        Ut = (static_cast<long double>(keys[size - 1 - D]) - static_cast<long double>(keys[D])) / (static_cast<double>(L - 2)) + 1e-6;
                    }

                    if (D * 3 <= size) {
                        stats.fmcd_success_times++;

                        node->model.a = 1.0 / Ut;
                        node->model.b = (L - node->model.a * (static_cast<long double>(keys[size - 1 - D]) + static_cast<long double>(keys[D]))) / 2;

                        RT_ASSERT(isfinite(node->model.a));
                        RT_ASSERT(isfinite(node->model.b));

                        node->num_items = L;
                    }
                    else {
                        stats.fmcd_broken_times++;

                        int mid1_pos = (size - 1) / 3;
                        int mid2_pos = (size - 1) * 2 / 3;

                        RT_ASSERT(0 <= mid1_pos);
                        RT_ASSERT(mid1_pos < mid2_pos);
                        RT_ASSERT(mid2_pos < size - 1);

                        const long double mid1_key = (static_cast<long double>(keys[mid1_pos]) + static_cast<long double>(keys[mid1_pos + 1])) / 2;
                        const long double mid2_key = (static_cast<long double>(keys[mid2_pos]) + static_cast<long double>(keys[mid2_pos + 1])) / 2;

                        node->num_items = size * static_cast<int>(BUILD_GAP_CNT + 1);
                        const double mid1_target = mid1_pos * static_cast<int>(BUILD_GAP_CNT + 1) + static_cast<int>(BUILD_GAP_CNT + 1) / 2;
                        const double mid2_target = mid2_pos * static_cast<int>(BUILD_GAP_CNT + 1) + static_cast<int>(BUILD_GAP_CNT + 1) / 2;

                        node->model.a = (mid2_target - mid1_target) / (mid2_key - mid1_key);
                        node->model.b = mid1_target - node->model.a * mid1_key;

                        RT_ASSERT(isfinite(node->model.a));
                        RT_ASSERT(isfinite(node->model.b));
                    }
                }

                RT_ASSERT(node->model.a >= 0);

                const int lr_remains = static_cast<int>(size * BUILD_LR_REMAIN);
                node->model.b += lr_remains;
                node->num_items += lr_remains * 2;

                if (size > 1e6) {
                    node->fixed = 1;
                }

                node->items = new_items(node->num_items);
                const int bitmap_size = BITMAP_SIZE(node->num_items);
                node->none_bitmap = new_bitmap(bitmap_size);
                node->child_bitmap = new_bitmap(bitmap_size);

                memset(node->none_bitmap, 0xff, sizeof(bitmap_t) * bitmap_size);
                memset(node->child_bitmap, 0, sizeof(bitmap_t) * bitmap_size);

                for (int item_i = PREDICT_POS(node, keys[0]), offset = 0; offset < size;) {
                    int next = offset + 1, next_i = -1;

                    while (next < size) {
                        next_i = PREDICT_POS(node, keys[next]);
                        if (next_i == item_i) {
                            next++;
                        }
                        else {
                            break;
                        }
                    }

                    if (next == offset + 1) {
                        BITMAP_CLEAR(node->none_bitmap, item_i);
                        node->items[item_i].comp.data.key = keys[offset];
                        node->items[item_i].comp.data.value = values[offset];
                    }
                    else {
                        // ASSERT(next - offset <= (size + 2) / 3);
                        BITMAP_CLEAR(node->none_bitmap, item_i);
                        BITMAP_SET(node->child_bitmap, item_i);
                        node->items[item_i].comp.child = new_nodes(1);
                        s.push((Segment){begin + offset, begin + next, level + 1, node->items[item_i].comp.child});
                    }

                    if (next >= size) {
                        break;
                    }
                    else {
                        item_i = next_i;
                        offset = next;
                    }
                }
            }
        }

        return ret;
    }

    void destory_pending() {
        while (!pending_two.empty()) {
            Node *node = pending_two.top();
            pending_two.pop();

            delete_items(node->items, node->num_items);
            const int bitmap_size = BITMAP_SIZE(node->num_items);
            delete_bitmap(node->none_bitmap, bitmap_size);
            delete_bitmap(node->child_bitmap, bitmap_size);
            delete_nodes(node, 1);
        }
    }

    void destroy_tree(Node *root) {
        std::stack<Node *> s;
        s.push(root);

        while (!s.empty()) {
            Node *node = s.top();
            s.pop();

            for (int i = 0; i < node->num_items; i++) {
                if (BITMAP_GET(node->child_bitmap, i) == 1) {
                    s.push(node->items[i].comp.child);
                }
            }

            if (node->is_two) {
                RT_ASSERT(node->build_size == 2);
                // RT_ASSERT(node->num_items == 8);

                node->size = 2;
                node->num_inserts = node->num_insert_to_data = 0;
                node->none_bitmap[0] = 0xff;
                node->child_bitmap[0] = 0;
                pending_two.push(node);
            }
            else {
                delete_items(node->items, node->num_items);
                const int bitmap_size = BITMAP_SIZE(node->num_items);
                delete_bitmap(node->none_bitmap, bitmap_size);
                delete_bitmap(node->child_bitmap, bitmap_size);
                delete_nodes(node, 1);
            }
        }
    }

    void scan_and_destory_tree(Node *_root, T *keys, P *values, bool destory = true) {
        typedef std::pair<int, Node *> Segment; // <begin, Node *>
        std::stack<Segment> s;

        s.push(Segment(0, _root));

        while (!s.empty()) {
            int begin = s.top().first;
            Node *node = s.top().second;
            const int SHOULD_END_POS = begin + node->size;

            s.pop();

            for (int i = 0; i < node->num_items; i++) {
                if (BITMAP_GET(node->none_bitmap, i) == 0) {
                    if (BITMAP_GET(node->child_bitmap, i) == 0) {
                        keys[begin] = node->items[i].comp.data.key;
                        values[begin] = node->items[i].comp.data.value;
                        begin++;
                    }
                    else {
                        s.push(Segment(begin, node->items[i].comp.child));
                        begin += node->items[i].comp.child->size;
                    }
                }
            }

            // RT_ASSERT(SHOULD_END_POS == begin);

            if (destory) {
                if (node->is_two) {
                    RT_ASSERT(node->build_size == 2);
                    // RT_ASSERT(node->num_items == 8);

                    node->size = 2;
                    node->num_inserts = node->num_insert_to_data = 0;
                    node->none_bitmap[0] = 0xff;
                    node->child_bitmap[0] = 0;
                    pending_two.push(node);
                }
                else {
                    delete_items(node->items, node->num_items);
                    const int bitmap_size = BITMAP_SIZE(node->num_items);
                    delete_bitmap(node->none_bitmap, bitmap_size);
                    delete_bitmap(node->child_bitmap, bitmap_size);
                    delete_nodes(node, 1);
                }
            }
        }
    }

    Node *insert_tree(Node *_node, const T &key, const P &value, bool *ok = nullptr) {
        constexpr int MAX_DEPTH = 128;
        Node *path[MAX_DEPTH];
        int path_size = 0;
        int insert_to_data = 0;

        for (Node *node = _node;;) {
            RT_ASSERT(path_size < MAX_DEPTH);

            path[path_size++] = node;
            node->size++;
            node->num_inserts++;

            int pos = PREDICT_POS(node, key);

            if (BITMAP_GET(node->none_bitmap, pos) == 1) { // None
                BITMAP_CLEAR(node->none_bitmap, pos);
                node->items[pos].comp.data.key = key;
                node->items[pos].comp.data.value = value;
                break;
            }
            else if (BITMAP_GET(node->child_bitmap, pos) == 0) { // Data
                BITMAP_SET(node->child_bitmap, pos);
                node->items[pos].comp.child = build_tree_two(key, value, node->items[pos].comp.data.key, node->items[pos].comp.data.value);
                insert_to_data = 1;
                break;
            }
            else { // Node
                node = node->items[pos].comp.child;
                this->conflict++;
            }
        }

        for (int i = 0; i < path_size; i++) {
            path[i]->num_insert_to_data += insert_to_data;
        }

        for (int i = 0; i < path_size; i++) {
            Node *node = path[i];
            const int num_inserts = node->num_inserts;
            const int num_insert_to_data = node->num_insert_to_data;
            const bool need_rebuild = node->fixed == 0 && node->size >= node->build_size * 2 && node->size >= 64 && num_insert_to_data * 10 >= num_inserts;

            if (need_rebuild) {
                const int ESIZE = node->size;
                T *keys = new T[ESIZE];
                P *values = new P[ESIZE];

                num_rebuild++;

#if COLLECT_TIME
                auto start_time_scan = std::chrono::high_resolution_clock::now();
#endif
                scan_and_destory_tree(node, keys, values);
#if COLLECT_TIME
                auto end_time_scan = std::chrono::high_resolution_clock::now();
                auto duration_scan = end_time_scan - start_time_scan;
                stats.time_scan_and_destory_tree += std::chrono::duration_cast<std::chrono::nanoseconds>(duration_scan).count() * 1e-9;
#endif

#if COLLECT_TIME
                auto start_time_build = std::chrono::high_resolution_clock::now();
#endif
                Node *new_node = build_tree_bulk(keys, values, ESIZE);
#if COLLECT_TIME
                auto end_time_build = std::chrono::high_resolution_clock::now();
                auto duration_build = end_time_build - start_time_build;
                stats.time_build_tree_bulk += std::chrono::duration_cast<std::chrono::nanoseconds>(duration_build).count() * 1e-9;
#endif

                delete[] keys;
                delete[] values;

                path[i] = new_node;

                if (i > 0) {
                    int pos = PREDICT_POS(path[i - 1], key);
                    path[i - 1]->items[pos].comp.child = new_node;
                }

                break;
            }
        }

        if (ok) {
            *ok = true;
        }
        return path[0];
    }

    // SATISFY_LOWER = true means all the keys in the subtree of `node` are no less than to `lower`.
    template<bool SATISFY_LOWER>
    int range_core_len(std::pair<T, P> *results, int pos, Node *node, const T &lower, int len) {
        this->count_map.insert({node, Count()});

        if constexpr(SATISFY_LOWER)
        {
            int bit_pos = 0;
            const bitmap_t *none_bitmap = node->none_bitmap;

            this->count_map.insert({node, Count()});

            while (bit_pos < node->num_items) {
                bitmap_t not_none = ~(*none_bitmap);
                while (not_none) {
                    int latest_pos = BITMAP_NEXT_1(not_none);
                    not_none ^= 1 << latest_pos;
                    int i = bit_pos + latest_pos;

                    if (BITMAP_GET(node->child_bitmap, i) == 0) {
                        results[pos] = {node->items[i].comp.data.key, node->items[i].comp.data.value};
                        // __builtin_prefetch((void*)&(node->items[i].comp.data.key) + 64);
                        pos++;
                    }
                    else {
                        this->count_map[node].down++;
                        pos = range_core_len<true>(results, pos, node->items[i].comp.child, lower, len);
                        this->count_map[node].up++;
                    }
                    if (pos >= len) {
                        return pos;
                    }
                }

                bit_pos += BITMAP_WIDTH;
                none_bitmap++;
            }

            return pos;
        }
        else {
            int lower_pos = PREDICT_POS(node, lower);
            if (BITMAP_GET(node->none_bitmap, lower_pos) == 0) {
                if (BITMAP_GET(node->child_bitmap, lower_pos) == 0) {
                    if (node->items[lower_pos].comp.data.key >= lower) {
                        results[pos] = {node->items[lower_pos].comp.data.key, node->items[lower_pos].comp.data.value};
                        pos++;
                    }
                }
                else {
                    pos = range_core_len<false>(results, pos, node->items[lower_pos].comp.child, lower, len);
                }
                if (pos >= len) {
                    return pos;
                }
            }
            if (lower_pos + 1 >= node->num_items) {
                return pos;
            }

            int bit_pos = (lower_pos + 1) / BITMAP_WIDTH * BITMAP_WIDTH;
            const bitmap_t *none_bitmap = node->none_bitmap + bit_pos / BITMAP_WIDTH;

            while (bit_pos < node->num_items) {
                bitmap_t not_none = ~(*none_bitmap);
                while (not_none) {
                    int latest_pos = BITMAP_NEXT_1(not_none);
                    not_none ^= 1 << latest_pos;
                    int i = bit_pos + latest_pos;

                    if (i <= lower_pos)
                        continue;
                    if (BITMAP_GET(node->child_bitmap, i) == 0) {
                        results[pos] = {node->items[i].comp.data.key, node->items[i].comp.data.value};
                        // __builtin_prefetch((void*)&(node->items[i].comp.data.key) + 64);
                        pos++;
                    }
                    else {
                        this->count_map[node].down++;
                        pos = range_core_len<true>(results, pos, node->items[i].comp.child, lower, len);
                        this->count_map[node].up++;
                    }
                    if (pos >= len) {
                        return pos;
                    }
                }
                bit_pos += BITMAP_WIDTH;
                none_bitmap++;
            }
            return pos;
        }
    }
};

#endif // __LIPP_H__
