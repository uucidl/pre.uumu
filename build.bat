setlocal

fxc -T ps_4_0 -E main -Fh flat_ps.h -Vn d3d11_flat_ps_bc flat_ps.hlsl -nologo
if %ERRORLEVEL% neq 0 goto error_exit

fxc -T vs_4_0 -E main -Fh ortho_vs.h -Vn d3d11_ortho_vs_bc ortho_vs.hlsl -nologo
if %ERRORLEVEL% neq 0 goto error_exit

cl -Fe:mu_test_d3d11.exe pervognsen_mu.cpp mu_test_d3d11_unit.c -Z7 -Od -W1 -nologo ^
   -link -SUBSYSTEM:CONSOLE
if %ERRORLEVEL% neq 0 goto error_exit

cl -Fe:mu_test_gl.exe -DMU_D3D11_ENABLED=0 pervognsen_mu.cpp mu_test_gl_unit.c -Z7 -Od -W1 -nologo ^
   -link -SUBSYSTEM:CONSOLE
if %ERRORLEVEL% neq 0 goto error_exit

cl -Fe:mu_lat_test.exe mu_lat_test_unit.c mu_lat_test_win32.cpp ^
   pervognsen_mu.cpp ^
   -Z7 -Od -W1 -nologo -EHsc ^
   -link -SUBSYSTEM:CONSOLE
if %ERRORLEVEL% neq 0 goto error_exit

exit /b 0

:error_exit
endlocal
exit /b 1
