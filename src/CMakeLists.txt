file(GLOB SRCS
    tasks/*.cpp
    GUI/*.cpp
    sensors/*.cpp
    sensors/drivers/*.cpp
    motor/*.cpp
    motor/dshot/*.c
    app/*.cpp
    )

file(GLOB INCL
    tasks
    GUI
    sensors
    sensors/drivers
    motor
    motor/dshot
    app
    )

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS ${INCL}
                       REQUIRES driver freertos esp-dsp)