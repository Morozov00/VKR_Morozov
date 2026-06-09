cmake_minimum_required(VERSION 3.19)
project(motorstand LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort)

qt_standard_project_setup()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include intrin.h")

qt_add_executable(motorstand
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    serialmanager.h
    serialmanager.cpp
    joystickworker.h
    joystickworker.cpp
)
# SDL2
set(SDL2_PATH "C:/Projects/SDL2")
target_include_directories(motorstand PRIVATE ${SDL2_PATH}/include/SDL2)
target_compile_definitions(motorstand PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(motorstand
        PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        ${SDL2_PATH}/lib/libSDL2main.a
        ${SDL2_PATH}/lib/libSDL2.dll.a
)


install(TARGETS motorstand
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET motorstand
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
