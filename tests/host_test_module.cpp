#ifdef _WIN32
#define XS_TEST_EXPORT __declspec(dllexport)
#else
#define XS_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" XS_TEST_EXPORT int xs_host_test_value() { return 42; }
