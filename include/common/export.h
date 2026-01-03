#ifndef EYAKV_EXPORT_H_
#define EYAKV_EXPORT_H_

#ifdef _WIN32
#ifdef EYAKV_LOGGER_EXPORTS
#define EYAKV_LOGGER_API __declspec(dllexport)
#else
#define EYAKV_LOGGER_API __declspec(dllimport)
#endif

#ifdef EYAKV_COMMON_EXPORTS
#define EYAKV_COMMON_API __declspec(dllexport)
#else
#define EYAKV_COMMON_API __declspec(dllimport)
#endif

#ifdef EYAKV_STORAGE_EXPORTS
#define EYAKV_STORAGE_API __declspec(dllexport)
#else
#define EYAKV_STORAGE_API __declspec(dllimport)
#endif

#ifdef EYAKV_NETWORK_EXPORTS
#define EYAKV_NETWORK_API __declspec(dllexport)
#else
#define EYAKV_NETWORK_API __declspec(dllimport)
#endif

#ifdef EYAKV_RAFT_EXPORTS
#define EYAKV_RAFT_API __declspec(dllexport)
#else
#define EYAKV_RAFT_API __declspec(dllimport)
#endif

#ifdef EYAKV_STARTER_EXPORTS
#define EYAKV_STARTER_API __declspec(dllexport)
#else
#define EYAKV_STARTER_API __declspec(dllimport)
#endif
#else
#define EYAKV_LOGGER_API __attribute__((visibility("default")))
#define EYAKV_COMMON_API __attribute__((visibility("default")))
#define EYAKV_STORAGE_API __attribute__((visibility("default")))
#define EYAKV_NETWORK_API __attribute__((visibility("default")))
#define EYAKV_RAFT_API __attribute__((visibility("default")))
#define EYAKV_STARTER_API __attribute__((visibility("default")))
#endif

#endif // EYAKV_EXPORT_H_
