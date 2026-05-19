# CMake generated Testfile for 
# Source directory: C:/Users/kurtu/OneDrive/Belgeler/uartbinlib
# Build directory: C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(uartbin_tests "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/build/Debug/uartbin_tests.exe")
  set_tests_properties(uartbin_tests PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;15;add_test;C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(uartbin_tests "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/build/Release/uartbin_tests.exe")
  set_tests_properties(uartbin_tests PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;15;add_test;C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(uartbin_tests "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/build/MinSizeRel/uartbin_tests.exe")
  set_tests_properties(uartbin_tests PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;15;add_test;C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(uartbin_tests "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/build/RelWithDebInfo/uartbin_tests.exe")
  set_tests_properties(uartbin_tests PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;15;add_test;C:/Users/kurtu/OneDrive/Belgeler/uartbinlib/CMakeLists.txt;0;")
else()
  add_test(uartbin_tests NOT_AVAILABLE)
endif()
