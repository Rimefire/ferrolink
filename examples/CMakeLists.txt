# Manual two-process test binaries — run `producer` in one terminal and
# `consumer` in another to exercise the real cross-process IPC path.
# These are NOT part of the automated test suite (see tests/test_transport.cpp
# for the fork()-based automated equivalent); they exist for hands-on,
# eyeball verification during development.
add_executable(producer producer.cpp)
target_link_libraries(producer PRIVATE ferrolink::ipc ferrolink::options)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE ferrolink::ipc ferrolink::options)
