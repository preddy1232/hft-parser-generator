# test_in_docker.ps1
# Automates building the Docker image and running the C++ tests inside it.

Write-Host "Building Ubuntu C++ Builder Image..." -ForegroundColor Cyan
docker build -t hft-parser-env .

if ($LASTEXITCODE -ne 0) {
    Write-Host "Docker build failed." -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "`nCompiling and Running Tests inside Docker..." -ForegroundColor Cyan
# Mount current directory, run cmake, build, and execute the tests
docker run --rm -v "${PWD}:/app" hft-parser-env /bin/bash -c "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`$(nproc) && ./build/itch_tests && ./build/ouch_tests && ./build/itch_bench && ./build/itch_lob_bench"

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n✅ All tests passed successfully!" -ForegroundColor Green
} else {
    Write-Host "`n❌ Tests failed or did not compile." -ForegroundColor Red
}
