#include <gtest/gtest.h>

#include <cstdint>

#include "KOReaderSyncClient.h"

// A TLS handshake is only paid once per connection and only over https. The
// handshake floors must therefore never gate a plain LAN server or a request
// riding an already-open session: those allocate request buffers only.
TEST(KOReaderSyncClientHeapFloor, RequiresAHandshakeForANewHttpsRequest) {
  EXPECT_EQ(KOReaderSyncClient::heapFloorFor(/*isHttps=*/true, /*connectionLive=*/false),
            KOReaderSyncClient::HeapFloor::Handshake);
}

TEST(KOReaderSyncClientHeapFloor, ReusesTheSmallFloorForPlainHttp) {
  EXPECT_EQ(KOReaderSyncClient::heapFloorFor(/*isHttps=*/false, /*connectionLive=*/false),
            KOReaderSyncClient::HeapFloor::Request);
}

TEST(KOReaderSyncClientHeapFloor, ReusesTheSmallFloorOverAnOpenSession) {
  EXPECT_EQ(KOReaderSyncClient::heapFloorFor(/*isHttps=*/true, /*connectionLive=*/true),
            KOReaderSyncClient::HeapFloor::Request);
  EXPECT_EQ(KOReaderSyncClient::heapFloorFor(/*isHttps=*/false, /*connectionLive=*/true),
            KOReaderSyncClient::HeapFloor::Request);
}
