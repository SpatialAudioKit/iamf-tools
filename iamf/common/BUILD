package(default_visibility = ["//iamf:__subpackages__"])

cc_library(
    name = "read_bit_buffer",
    srcs = ["read_bit_buffer.cc"],
    hdrs = ["read_bit_buffer.h"],
    deps = [
        "//iamf/common/utils:bit_buffer_util",
        "//iamf/common/utils:macros",
        "//iamf/obu:types",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "write_bit_buffer",
    srcs = ["write_bit_buffer.cc"],
    hdrs = ["write_bit_buffer.h"],
    deps = [
        "//iamf/cli:leb_generator",
        "//iamf/common/utils:bit_buffer_util",
        "//iamf/common/utils:macros",
        "//iamf/obu:types",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)
