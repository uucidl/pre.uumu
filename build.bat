setlocal
cl -Fe:mu_test.exe pervognsen_mu.cpp mu_test_unit.c -Z7 -Od -W1 -nologo ^
   -link -SUBSYSTEM:CONSOLE

cl -Fe:mu_lat_test.exe mu_lat_test_unit.c mu_lat_test_win32.cpp ^
   pervognsen_mu.cpp ^
   -Z7 -Od -W1 -nologo -EHsc ^
   -link -SUBSYSTEM:CONSOLE


