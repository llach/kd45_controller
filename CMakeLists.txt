cmake_minimum_required(VERSION 2.8.3)
project(kd45_controller)

# Default to C++14
if ("${CMAKE_VERSION}" VERSION_LESS "3.1")
    add_compile_options(-std=c++14)
else ()
    set(CMAKE_CXX_STANDARD 14)
endif ()

find_package(Eigen3 REQUIRED)

find_package(catkin REQUIRED COMPONENTS
        roscpp
        tactile_msgs
        controller_interface
        joint_trajectory_controller
        )

catkin_package(
        CATKIN_DEPENDS
        joint_trajectory_controller
        INCLUDE_DIRS include
        LIBRARIES ${PROJECT_NAME}
)

include_directories(include ${catkin_INCLUDE_DIRS} ${EIGEN3_INCLUDE_DIRS})

add_library(${PROJECT_NAME}
        include/tactile_sensor.h
        include/tactile_sensor_impl.h
        include/kd45_controller.h
        include/kd45_controller_impl.h

        src/kd45_controller.cpp
        )


target_link_libraries(${PROJECT_NAME}
        ${catkin_LIBRARIES}
        ${EIGEN3_LIBRARIES}
        )

# Install
install(DIRECTORY include
        DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})

# Install library
install(TARGETS ${PROJECT_NAME}
        ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
        LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
        RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION}
        )

install(DIRECTORY launch config
        DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
        )

install(FILES ${PROJECT_NAME}_plugins.xml
        DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})