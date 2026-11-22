# The player's OS-free core (core/*.cpp), as the desktop build compiles it. Its headers include
# "core/..." and "third_party/minimp3/...": the repository's root is the include directory.
set(root "${CMAKE_CURRENT_LIST_DIR}/../../../..")
file(GLOB sources "${root}/core/*.cpp")

idf_component_register(
    SRCS ${sources}
    INCLUDE_DIRS "${root}"
    REQUIRES prism
)
