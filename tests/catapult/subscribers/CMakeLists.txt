cmake_minimum_required(VERSION 3.2)

catapult_test_executable_target(tests.catapult.subscribers cache test)
target_link_libraries(tests.catapult.subscribers catapult.config)
