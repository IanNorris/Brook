#include "../../kernel/src/posix_mqueue_order.h"

#include <cassert>

int main() {
    brook::MqueueOrder order;
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const unsigned low = order.Push(2, 1);
        const unsigned first = order.Push(3, 10);
        const unsigned second = order.Push(4, 10);
        assert(low != first && first != second && low != second);
        assert(order.count == 3 && order.Bytes() == 9);
        const auto a = order.Pop();
        assert(a.slot == first && a.priority == 10 && a.length == 3);
        const unsigned top = order.Push(8, 11);
        assert(top == first);
        assert(order.Pop().slot == top);
        assert(order.Pop().slot == second);
        assert(order.Pop().slot == low);
        assert(order.count == 0 && order.Bytes() == 0);
    }
    for (unsigned i = 0; i < brook::MqueueOrder::MaxMessages; ++i)
        assert(order.Push(i, i) == i);
    for (unsigned i = brook::MqueueOrder::MaxMessages; i != 0; --i)
        assert(order.Pop().slot == i - 1);
}
