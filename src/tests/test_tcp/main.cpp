// test_tcp — Host-native unit tests for the TCP state machine.
//
// Tests TcpProcessSegment() in isolation — no networking, no kernel.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

// Pull in network types (only needs <stdint.h>)
#include "net.h"
#include "tcp.h"

using namespace brook;

namespace brook {
    volatile uint64_t g_lapicTickCount = 0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
}

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(expr)                                       \
    do {                                                        \
        if (!(expr)) {                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n",                \
                    __FILE__, __LINE__, #expr);                 \
            g_fail++;                                           \
        } else { g_pass++; }                                    \
    } while (0)

#define ASSERT_EQ(a, b)                                         \
    do {                                                        \
        auto _a = (a); auto _b = (b);                           \
        if (_a != _b) {                                         \
            fprintf(stderr, "FAIL %s:%d: %s != %s (%d != %d)\n", \
                    __FILE__, __LINE__, #a, #b,                 \
                    (int)_a, (int)_b);                          \
            g_fail++;                                           \
        } else { g_pass++; }                                    \
    } while (0)

// Helper: create a zeroed socket in SynSent state
static Socket MakeSynSentSocket()
{
    Socket s = {};
    s.type       = SOCK_STREAM;
    s.tcpState   = TcpState::SynSent;
    s.tcpSndNxt  = 1000;
    s.tcpSndIss  = 1000;
    s.tcpSndUna  = 1000;
    s.tcpRcvNxt  = 0;
    s.connected  = false;
    s.tcpFinRecv = false;
    return s;
}

// Helper: create a socket in Established state
static Socket MakeEstablishedSocket()
{
    Socket s = {};
    s.type       = SOCK_STREAM;
    s.tcpState   = TcpState::Established;
    s.tcpSndNxt  = 1001;
    s.tcpSndIss  = 1000;
    s.tcpSndUna  = 1001;
    s.tcpRcvNxt  = 5001;
    s.connected  = true;
    s.tcpFinRecv = false;
    return s;
}

// ---------------------------------------------------------------------------
// Test: SYN-ACK transitions SynSent → Established
// ---------------------------------------------------------------------------
static void TestSynAck()
{
    Socket s = MakeSynSentSocket();

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5000, /*ack=*/1001,
        TCP_SYN | TCP_ACK,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::Established);
    ASSERT_TRUE(s.connected);
    ASSERT_EQ(s.tcpRcvNxt, 5001u);
    ASSERT_EQ(s.tcpSndUna, 1001u);
    ASSERT_TRUE(act.sendAck);      // must ACK to complete handshake
    ASSERT_TRUE(!act.enqueueData);
    printf("  TestSynAck: OK\n");
}

// ---------------------------------------------------------------------------
// Test: RST in SynSent → Closed
// ---------------------------------------------------------------------------
static void TestSynRst()
{
    Socket s = MakeSynSentSocket();

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/0, /*ack=*/0,
        TCP_RST,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::Closed);
    ASSERT_TRUE(!s.connected);
    ASSERT_TRUE(!act.sendAck);
    printf("  TestSynRst: OK\n");
}

// ---------------------------------------------------------------------------
// Test: Receive data in Established
// ---------------------------------------------------------------------------
static void TestEstablishedReceive()
{
    Socket s = MakeEstablishedSocket();
    uint8_t data[] = "Hello, TCP!";
    uint32_t dataLen = sizeof(data) - 1; // exclude NUL

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1001,
        TCP_ACK,
        data, dataLen);

    ASSERT_EQ(s.tcpState, TcpState::Established);
    ASSERT_TRUE(s.connected);
    ASSERT_EQ(s.tcpRcvNxt, 5001u + dataLen);
    ASSERT_TRUE(act.sendAck);
    ASSERT_TRUE(act.enqueueData);
    ASSERT_EQ(act.dataLen, dataLen);
    ASSERT_TRUE(act.dataPtr == data);
    printf("  TestEstablishedReceive: OK\n");
}

// ---------------------------------------------------------------------------
// Test: Out-of-order data in Established → ACK but no enqueue
// ---------------------------------------------------------------------------
static void TestEstablishedOOO()
{
    Socket s = MakeEstablishedSocket();
    uint8_t data[] = "ooo";

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/9999, /*ack=*/1001,  // wrong seq
        TCP_ACK,
        data, 3);

    ASSERT_EQ(s.tcpState, TcpState::Established);
    ASSERT_TRUE(act.sendAck);       // ACK with expected seq
    ASSERT_TRUE(!act.enqueueData);  // don't enqueue out-of-order
    ASSERT_EQ(s.tcpRcvNxt, 5001u);  // unchanged
    printf("  TestEstablishedOOO: OK\n");
}

// ---------------------------------------------------------------------------
// Test: RST in Established → Closed
// ---------------------------------------------------------------------------
static void TestEstablishedRst()
{
    Socket s = MakeEstablishedSocket();

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1001,
        TCP_RST,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::Closed);
    ASSERT_TRUE(!s.connected);
    ASSERT_TRUE(s.tcpRstRecv);      // RST, distinct from FIN (ECONNRESET vs EOF)
    ASSERT_TRUE(!s.tcpFinRecv);
    ASSERT_TRUE(!act.sendAck);
    printf("  TestEstablishedRst: OK\n");
}

// ---------------------------------------------------------------------------
// Test: FIN in Established → CloseWait
// ---------------------------------------------------------------------------
static void TestEstablishedFin()
{
    Socket s = MakeEstablishedSocket();

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1001,
        TCP_FIN | TCP_ACK,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::CloseWait);
    ASSERT_TRUE(s.tcpFinRecv);
    ASSERT_EQ(s.tcpRcvNxt, 5002u);  // FIN consumes 1 sequence number
    ASSERT_TRUE(act.sendAck);
    printf("  TestEstablishedFin: OK\n");
}

// ---------------------------------------------------------------------------
// Test: Data + FIN in Established → enqueue data, then CloseWait
// ---------------------------------------------------------------------------
static void TestEstablishedDataAndFin()
{
    Socket s = MakeEstablishedSocket();
    uint8_t data[] = "BYE";

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1001,
        TCP_FIN | TCP_ACK,
        data, 3);

    ASSERT_EQ(s.tcpState, TcpState::CloseWait);
    ASSERT_TRUE(s.tcpFinRecv);
    ASSERT_TRUE(act.enqueueData);
    ASSERT_EQ(act.dataLen, 3u);
    // rcvNxt should advance by data + FIN
    ASSERT_EQ(s.tcpRcvNxt, 5001u + 3 + 1);
    ASSERT_TRUE(act.sendAck);
    printf("  TestEstablishedDataAndFin: OK\n");
}

// ---------------------------------------------------------------------------
// Test: ACK in FinWait1 → FinWait2
// ---------------------------------------------------------------------------
static void TestFinWait1ToFinWait2()
{
    Socket s = MakeEstablishedSocket();
    s.tcpState = TcpState::FinWait1;

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1002,
        TCP_ACK,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::FinWait2);
    ASSERT_EQ(s.tcpSndUna, 1002u);
    ASSERT_TRUE(!act.sendAck);
    printf("  TestFinWait1ToFinWait2: OK\n");
}

// ---------------------------------------------------------------------------
// Test: FIN+ACK in FinWait1 → TimeWait (simultaneous close)
// ---------------------------------------------------------------------------
static void TestFinWait1SimultaneousClose()
{
    Socket s = MakeEstablishedSocket();
    s.tcpState = TcpState::FinWait1;

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1002,
        TCP_FIN | TCP_ACK,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::TimeWait);
    ASSERT_EQ(s.tcpRcvNxt, 5002u);
    ASSERT_TRUE(act.sendAck);
    printf("  TestFinWait1SimultaneousClose: OK\n");
}

// ---------------------------------------------------------------------------
// Test: FIN in FinWait2 → TimeWait
// ---------------------------------------------------------------------------
static void TestFinWait2ToTimeWait()
{
    Socket s = MakeEstablishedSocket();
    s.tcpState = TcpState::FinWait2;

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1001,
        TCP_FIN | TCP_ACK,
        nullptr, 0);

    ASSERT_EQ(s.tcpState, TcpState::TimeWait);
    ASSERT_EQ(s.tcpRcvNxt, 5002u);
    ASSERT_TRUE(act.sendAck);
    printf("  TestFinWait2ToTimeWait: OK\n");
}

// ---------------------------------------------------------------------------
// Test: ACK in LastAck → Closed
// ---------------------------------------------------------------------------
static void TestLastAckToClosed()
{
    Socket s = MakeEstablishedSocket();
    s.tcpState = TcpState::LastAck;

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1002,
        TCP_ACK,
        nullptr, 0);
    (void)act;

    ASSERT_EQ(s.tcpState, TcpState::Closed);
    printf("  TestLastAckToClosed: OK\n");
}

// ---------------------------------------------------------------------------
// Test: ACK update in CloseWait
// ---------------------------------------------------------------------------
static void TestCloseWaitAckUpdate()
{
    ASSERT_TRUE(TcpCanSendData(TcpState::Established));
    ASSERT_TRUE(TcpCanSendData(TcpState::CloseWait));
    ASSERT_TRUE(!TcpCanSendData(TcpState::Closed));
    ASSERT_TRUE(!TcpCanSendData(TcpState::LastAck));
    Socket s = MakeEstablishedSocket();
    s.tcpState = TcpState::CloseWait;

    TcpAction act = TcpProcessSegment(s,
        /*seq=*/5001, /*ack=*/1005,
        TCP_ACK,
        nullptr, 0);
    (void)act;

    ASSERT_EQ(s.tcpState, TcpState::CloseWait);
    ASSERT_EQ(s.tcpSndUna, 1005u);
    TcpProcessSegment(s, s.tcpRcvNxt, 0, TCP_RST, nullptr, 0);
    ASSERT_EQ(s.tcpState, TcpState::Closed);
    ASSERT_TRUE(s.tcpRstRecv);
    ASSERT_TRUE(!s.connected);
    printf("  TestCloseWaitAckUpdate: OK\n");
}

// ---------------------------------------------------------------------------
// Test: TcpChecksum produces correct result
// ---------------------------------------------------------------------------
static void TestChecksum()
{
    // Build a minimal TCP header
    uint8_t buf[20] = {};
    auto* tcp = reinterpret_cast<TcpHeader*>(buf);
    tcp->srcPort = htons(12345);
    tcp->dstPort = htons(80);
    tcp->seqNum  = htonl(1);
    tcp->ackNum  = htonl(0);
    tcp->dataOff = (5 << 4); // 20 bytes
    tcp->flags   = TCP_SYN;
    tcp->window  = htons(65535);
    tcp->checksum = 0;

    uint32_t srcIp = htonl(0x0A000201); // 10.0.2.1
    uint32_t dstIp = htonl(0x0A000202); // 10.0.2.2

    uint16_t cksum = TcpChecksum(srcIp, dstIp, buf, 20);

    // Set checksum and verify it validates to 0
    tcp->checksum = cksum;
    uint16_t verify = TcpChecksum(srcIp, dstIp, buf, 20);
    ASSERT_EQ(verify, 0u);
    printf("  TestChecksum: OK\n");
}

static void TestPassiveHandshake()
{
    auto passive=[] { Socket s=MakeEstablishedSocket();s.tcpState=TcpState::SynRecv;
                       s.connected=false;s.tcpLastRxTick=100;return s; };
    Socket s=passive();
    auto action=TcpProcessSegment(s,5000,0,TCP_SYN,nullptr,0);
    ASSERT_TRUE(action.sendSynAck);ASSERT_EQ(s.tcpState,TcpState::SynRecv);
    ASSERT_EQ(s.tcpSndNxt,1001u);ASSERT_EQ(s.tcpRcvNxt,5001u);
    ASSERT_EQ(s.tcpLastRxTick,100u);
    action=TcpProcessSegment(s,4999,0,TCP_SYN,nullptr,0);ASSERT_TRUE(!action.sendSynAck);
    action=TcpProcessSegment(s,5001,1000,TCP_ACK,nullptr,0);
    ASSERT_EQ(s.tcpState,TcpState::SynRecv);ASSERT_TRUE(!action.justConnected);
    s=passive();
    action=TcpProcessSegment(s,5001,1001,TCP_RST|TCP_ACK,nullptr,0);
    ASSERT_EQ(s.tcpState,TcpState::Closed);ASSERT_TRUE(s.tcpRstRecv&&!s.connected);
    ASSERT_TRUE(!action.justConnected&&!action.enqueueData);
    s=passive();const uint8_t request[]={1,2,3};
    action=TcpProcessSegment(s,5001,1001,TCP_ACK|TCP_FIN,request,3);
    ASSERT_TRUE(action.justConnected&&action.enqueueData&&action.sendAck);
    ASSERT_EQ(action.dataLen,3u);ASSERT_TRUE(action.dataPtr==request);
    ASSERT_EQ(s.tcpState,TcpState::CloseWait);ASSERT_TRUE(s.connected&&s.tcpFinRecv);
    ASSERT_EQ(s.tcpRcvNxt,5005u);
    s=passive();ASSERT_TRUE(!TcpPassiveOpenExpired(s,30099));
    ASSERT_TRUE(TcpPassiveOpenExpired(s,30100));
    s.tcpState=TcpState::Established;ASSERT_TRUE(!TcpPassiveOpenExpired(s,99999));
    s.tcpState=TcpState::CloseWait;ASSERT_TRUE(!TcpPassiveOpenExpired(s,99999));
    s=passive();s.tcpLastRxTick=~uint64_t{0}-100;
    ASSERT_TRUE(TcpPassiveOpenExpired(s,30000));
    std::puts("  TestPassiveHandshake: OK");
}

int main()
{
    uint8_t options[8] = {};
    ASSERT_EQ(TcpWriteSynOptions(TCP_ACK, 7, options), 0u);
    ASSERT_EQ(TcpWriteSynOptions(TCP_SYN | TCP_ACK, 7, options), 4u);
    ASSERT_EQ(options[0], 2); ASSERT_EQ(options[1], 4);
    ASSERT_EQ((options[2] << 8) | options[3], 1460);
    ASSERT_EQ(TcpWriteSynOptions(TCP_SYN, 7, options), 8u);
    ASSERT_EQ(options[4], 1); ASSERT_EQ(options[5], 3);
    ASSERT_EQ(options[6], 3); ASSERT_EQ(options[7], 7);
    ASSERT_TRUE(Ipv4IsLoopback(htonl(0x7f000001)));
    ASSERT_TRUE(Ipv4IsLoopback(htonl(0x7f123456)));
    ASSERT_TRUE(!Ipv4IsLoopback(htonl(0x0a00020f)));
    ASSERT_TRUE(!Ipv4IsLoopback(htonl(0x80000001)));
    ASSERT_EQ(Ipv4Source(0,htonl(0x7f000002),0),htonl(0x7f000001));
    ASSERT_EQ(Ipv4Source(0,htonl(0x08080808),htonl(0x0a00020f)),htonl(0x0a00020f));
    ASSERT_EQ(Ipv4Source(htonl(0x7f000002),htonl(0x7f000003),0),htonl(0x7f000002));
    printf("=== TCP State Machine Tests ===\n");

    ASSERT_TRUE(!TcpTimerElapsed(1000, 1001, 40));
    ASSERT_TRUE(!TcpTimerElapsed(1000, 2168, 60000));
    ASSERT_TRUE(!TcpTimerElapsed(1000, 1000, 40));
    ASSERT_TRUE(!TcpTimerElapsed(1039, 1000, 40));
    ASSERT_TRUE(TcpTimerElapsed(1040, 1000, 40));
    ASSERT_TRUE(!TcpTimerElapsed(60999, 1000, 60000));
    ASSERT_TRUE(TcpTimerElapsed(61000, 1000, 60000));
    ASSERT_TRUE(TcpTimerElapsed(30, ~uint64_t{0} - 9, 40));
    Socket rewound = {};
    rewound.tcpState = TcpState::SynRecv;
    rewound.tcpLastRxTick = 2168;
    ASSERT_TRUE(!TcpPassiveOpenExpired(rewound, 1000));

    Socket acceptable = {};
    for (auto state : {TcpState::Closed, TcpState::SynSent, TcpState::SynRecv,
                       TcpState::FinWait1, TcpState::FinWait2, TcpState::LastAck,
                       TcpState::TimeWait, TcpState::Listen}) {
        acceptable.tcpState = state; ASSERT_TRUE(!TcpCanAccept(acceptable));
    }
    acceptable.tcpState = TcpState::Established; ASSERT_TRUE(TcpCanAccept(acceptable));
    acceptable.tcpState = TcpState::CloseWait; acceptable.tcpFinRecv = true;
    ASSERT_TRUE(TcpCanAccept(acceptable));
    acceptable.tcpRstRecv = true; ASSERT_TRUE(!TcpCanAccept(acceptable));

    TestPassiveHandshake();
    TestSynAck();
    TestSynRst();
    TestEstablishedReceive();
    TestEstablishedOOO();
    TestEstablishedRst();
    TestEstablishedFin();
    TestEstablishedDataAndFin();
    TestFinWait1ToFinWait2();
    TestFinWait1SimultaneousClose();
    TestFinWait2ToTimeWait();
    TestLastAckToClosed();
    TestCloseWaitAckUpdate();
    TestChecksum();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
