#ifndef LABSTOR_SRC_CONFIG_CLIENT_DEFAULT_H_
#define LABSTOR_SRC_CONFIG_CLIENT_DEFAULT_H_
static inline const char* kHermesClientDefaultConfigStr =
"stop_daemon: false\n"
"path_inclusions: [\"/tmp/test_hermes\"]\n"
"path_exclusions: [\"/\"]\n"
"file_page_size: 1024KB\n"
"base_adapter_mode: kDefault\n"
"flushing_mode: kAsync\n"
"file_adapter_configs:\n"
"  - path: \"/\"\n"
"    page_size: 1MB\n"
"    mode: kDefault\n";
#endif  // LABSTOR_SRC_CONFIG_CLIENT_DEFAULT_H_