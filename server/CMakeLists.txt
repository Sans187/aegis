cmake_minimum_required(VERSION 3.20)

# 静态 CRT：让最终 exe 不依赖 MSVC 运行时 DLL（vcredist）。
# 必须在 project() 之前确立策略，配合 x64-windows-static 三元组实现全静态单文件。
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(aegis_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件含 UTF-8 中文注释；强制 MSVC 按 UTF-8 解析，否则会按系统代码页(GBK)误读导致语法错误。
# NOMINMAX：禁止 windows.h 定义 min/max 宏，否则会破坏 std::max/std::clamp。
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# 依赖（由 vcpkg 工具链提供，见 CMakePresets.json）
find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)   # libsodium：Argon2id 密码哈希
find_package(cryptopp CONFIG REQUIRED)            # RSA / RC4 / Base64（协议兼容）

file(GLOB_RECURSE AEGIS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(aegis_server ${AEGIS_SOURCES})

target_include_directories(aegis_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."          # 让 #include "proto/aegis_proto.h" 可用（客户端库与服务端共享）
)

target_link_libraries(aegis_server PRIVATE
    Drogon::Drogon
    unofficial-sodium::sodium
    cryptopp::cryptopp
)

# 把配置与 SQL 拷到可执行文件旁，方便运行
add_custom_command(TARGET aegis_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/config" "$<TARGET_FILE_DIR:aegis_server>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/sql" "$<TARGET_FILE_DIR:aegis_server>/sql"
)
