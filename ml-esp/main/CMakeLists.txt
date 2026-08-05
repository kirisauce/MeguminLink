file(GLOB_RECURSE __src ${CMAKE_CURRENT_LIST_DIR}/*.cc ${CMAKE_CURRENT_LIST_DIR}/*.c)
idf_component_register(SRCS ${__src}
                    INCLUDE_DIRS "./include")
