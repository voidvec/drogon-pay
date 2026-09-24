"""drogon-pay Conan recipe.

A reusable payment plugin library for the Drogon framework (WeChat Pay +
Alipay built in, custom channels via the PaymentChannel SPI).

Publishable recipe: `conan create . --build=missing` builds the static
library and packages headers + CMake config so consumers can either
`find_package(DrogonPay)` (CMakeDeps) or link `drogon-pay` directly.

Drogon version policy (v1.x): pinned to a single Drogon version. The library
and the host application MUST use the same Drogon version — static library +
C++ ABI make this a hard constraint. Drogon upgrades ship as minor releases.
"""

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, rmdir


class DrogonPayConan(ConanFile):
    name = "drogon-pay"
    version = "1.1.0"
    license = "MIT"
    url = "https://github.com/lucaswang420/drogon-pay"
    description = "A reusable payment plugin for the Drogon framework"
    topics = ("drogon", "payment", "wechat-pay", "alipay", "plugin")
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "libs/*",
        "LICENSE",
    )

    default_options = {
        # --- drogon options (required feature set for the pay plugin) ---
        "drogon/*:with_orm": True,
        "drogon/*:with_postgres": True,
        "drogon/*:with_redis": True,
        "drogon/*:with_ctl": True,
        "drogon/*:with_sqlite": True,
        "drogon/*:with_brotli": True,
    }

    def requirements(self):
        # hiredis/brotli/postgres/etc. are transitive via drogon; only the
        # packages whose headers/symbols this library uses directly are
        # declared here.
        self.requires("drogon/1.9.13", transitive_headers=True, transitive_libs=True)
        # Direct dependency (the library links OpenSSL::Crypto/SSL); force
        # resolves the version conflict against drogon's own openssl range.
        self.requires("openssl/3.5.7", force=True)
        # Public headers include <json/json.h> directly.
        self.requires("jsoncpp/1.9.5", transitive_headers=True, force=True)

    def layout(self):
        cmake_layout(self)
        # Pin the generators dir to a config-independent location so the
        # repo's CMakePresets.json can reference the toolchain at a stable
        # path (build/<preset>/build/generators/conan_toolchain.cmake) on
        # every platform. Without this, single-config generators would place
        # it under build/<build_type>/generators instead.
        self.folders.generators = "build/generators"

    def generate(self):
        tc = CMakeToolchain(self)
        # Package builds only ship the library: no example host, no tests.
        tc.cache_variables["DROGON_PAY_BUILD_EXAMPLES"] = False
        tc.cache_variables["DROGON_PAY_BUILD_TESTS"] = False
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", self.source_folder,
             os.path.join(self.package_folder, "licenses"))
        # The install step ships DrogonPayConfig.cmake for pure-CMake
        # consumers; Conan consumers get an equivalent config from CMakeDeps
        # (cmake_file_name/cmake_target_name below), so drop the in-package
        # one to avoid find_package picking a stale duplicate.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.libs = ["drogon-pay"]
        self.cpp_info.set_property("cmake_file_name", "DrogonPay")
        self.cpp_info.set_property("cmake_target_name", "DrogonPay::DrogonPay")
        self.cpp_info.requires = [
            "drogon::drogon",
            "openssl::openssl",
            "jsoncpp::jsoncpp",
        ]
