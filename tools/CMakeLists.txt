# DescriptorDump is portable and always builds -- CI runs it as a check.
add_subdirectory(DescriptorDump)

# The rest drive real Windows subsystems.
add_subdirectory(CorsairProbe)
add_subdirectory(HidTestClient)
add_subdirectory(LampArrayTest)
