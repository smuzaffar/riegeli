package(
    default_visibility = ["//visibility:public"],
    features = ["header_modules"],
)

licenses(["notice"])

cc_library(
    name = "digester",
    srcs = ["digester.cc"],
    hdrs = ["digester.h"],
    deps = [
        "//riegeli/base:arithmetic",
        "//riegeli/base:assert",
        "//riegeli/base:chain",
        "//riegeli/base:dependency",
        "//riegeli/base:type_traits",
        "//riegeli/base:types",
        "//riegeli/base:zeros",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/meta:type_traits",
        "@com_google_absl//absl/numeric:int128",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:cord",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "digesting_reader",
    srcs = ["digesting_reader.cc"],
    hdrs = ["digesting_reader.h"],
    deps = [
        ":digester",
        "//riegeli/base:arithmetic",
        "//riegeli/base:assert",
        "//riegeli/base:buffering",
        "//riegeli/base:chain",
        "//riegeli/base:dependency",
        "//riegeli/base:object",
        "//riegeli/base:status",
        "//riegeli/base:types",
        "//riegeli/bytes:reader",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/meta:type_traits",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:cord",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "digesting_writer",
    srcs = ["digesting_writer.cc"],
    hdrs = ["digesting_writer.h"],
    deps = [
        ":digester",
        "//riegeli/base:arithmetic",
        "//riegeli/base:assert",
        "//riegeli/base:buffering",
        "//riegeli/base:chain",
        "//riegeli/base:dependency",
        "//riegeli/base:object",
        "//riegeli/base:type_traits",
        "//riegeli/base:types",
        "//riegeli/bytes:null_writer",
        "//riegeli/bytes:reader",
        "//riegeli/bytes:writer",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/meta:type_traits",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:cord",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "crc32c_digester",
    hdrs = ["crc32c_digester.h"],
    deps = [
        ":digester",
        "//riegeli/base:arithmetic",
        "@com_google_absl//absl/strings",
        "@crc32c",
    ],
)

cc_library(
    name = "crc32_digester",
    srcs = ["crc32_digester.cc"],
    hdrs = ["crc32_digester.h"],
    deps = [
        ":digester",
        "//riegeli/base:arithmetic",
        "@com_google_absl//absl/strings",
        "@zlib",
    ],
)

cc_library(
    name = "adler32_digester",
    srcs = ["adler32_digester.cc"],
    hdrs = ["adler32_digester.h"],
    deps = [
        ":digester",
        "//riegeli/base:arithmetic",
        "@com_google_absl//absl/strings",
        "@zlib",
    ],
)
