add_executable(AutoProgressSyncTest AutoProgressSyncTest.cpp)
target_include_directories(AutoProgressSyncTest PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/lib/KOReaderSync)
target_link_libraries(AutoProgressSyncTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AutoProgressSyncTest)
