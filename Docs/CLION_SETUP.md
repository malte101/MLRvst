# Building with CLion

## Quick Fix for CMAKE_C_COMPILE_OBJECT Error

This error means CMake can't find your C compiler. Here's how to fix it:

## Solution 1: Install Build Tools (Recommended)

### macOS
```bash
xcode-select --install
```

### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install build-essential cmake
```

### Windows
Install **Visual Studio 2019 or later** with "Desktop development with C++" workload, or use **MSYS2/MinGW**.

## Solution 2: Configure CLion Toolchain

1. Open CLion → **Preferences/Settings**
2. Go to **Build, Execution, Deployment → Toolchains**
3. Ensure you have a valid toolchain configured:

### macOS
- **Toolchain**: System (or Xcode)
- **CMake**: Bundled or system CMake
- **Make**: /usr/bin/make
- **C Compiler**: /usr/bin/clang
- **C++ Compiler**: /usr/bin/clang++

### Linux
- **Toolchain**: Default
- **CMake**: Bundled or system CMake
- **Make**: /usr/bin/make
- **C Compiler**: /usr/bin/gcc or /usr/bin/clang
- **C++ Compiler**: /usr/bin/g++ or /usr/bin/clang++

### Windows (Visual Studio)
- **Toolchain**: Visual Studio
- **Architecture**: amd64
- **CMake**: Bundled
- **C Compiler**: Detected automatically
- **C++ Compiler**: Detected automatically

### Windows (MinGW)
- **Toolchain**: MinGW
- **CMake**: Bundled
- **Make**: mingw32-make
- **C Compiler**: gcc.exe
- **C++ Compiler**: g++.exe

4. Click **Apply** and **OK**

## Solution 3: Open Project in CLion

1. **File → Open** → Select your project root folder
2. CLion will ask to load CMake project → Click **OK**
3. Wait for CMake to configure
4. If it fails, check:
   - JUCE is cloned: `<repo-root>/JUCE/` should exist
   - Toolchain is configured (see Solution 2)

## Solution 4: Manual CMake Configuration

If CLion's automatic configuration fails:

1. Open **File → Settings → Build, Execution, Deployment → CMake**
2. Under **CMake options**, add:
   ```
   -DCMAKE_C_COMPILER=/usr/bin/clang
   -DCMAKE_CXX_COMPILER=/usr/bin/clang++
   ```
   (Adjust paths for your system)

3. Under **Build directory**, use: `cmake-build-debug` or `cmake-build-release`
4. Click **Apply** and **OK**
5. Right-click `CMakeLists.txt` → **Reload CMake Project**

## Step-by-Step Setup in CLion

### 1. Clone JUCE (if not done)
```bash
cd <repo-root>
git clone https://github.com/juce-framework/JUCE.git
```

### 2. Open in CLion
- **File → Open** → Navigate to your project root folder
- Click **OK**

### 3. Configure CMake
CLion should automatically:
- Detect CMakeLists.txt
- Configure the project

### 4. Select Build Configuration
- Top-right dropdown: Select **Debug** or **Release**

### 5. Build
- **Build → Build Project** (Cmd+F9 / Ctrl+F9)
- Or click the hammer icon 🔨

### 6. Build Targets
- Select `mlrVST_VST3` (and `mlrVST_AU` on macOS) from run/build configurations
- Click Build ▶️

## Common CLion Issues

### Issue: "Cannot find JUCE"

**Solution:**
```bash
cd <repo-root>
git clone https://github.com/juce-framework/JUCE.git
```
Then in CLion: **Tools → CMake → Reload CMake Project**

### Issue: "Toolchain is not configured"

**Solution:**
1. **Settings → Build, Execution, Deployment → Toolchains**
2. Click **+** to add a toolchain
3. Select appropriate toolchain for your OS
4. Make sure it detects compilers correctly

### Issue: "CMake version too old"

**Solution:**
1. **Settings → Build, Execution, Deployment → CMake**
2. Change **CMake executable** to a newer version
3. Or download CMake 3.22+ from cmake.org

### Issue: Build fails with linking errors

**Solution:**
Make sure JUCE is at least version 8.0.4:
```bash
cd JUCE
git pull origin master
git checkout 8.0.4  # or later
```

Then in CLion: **Tools → CMake → Reset Cache and Reload Project**

## Building Specific Targets

In CLion, you can build specific targets:

1. **Run → Edit Configurations**
2. Click **+** → **CMake Application**
3. Select target:
   - `mlrVST_VST3` - VST3 plugin
   - `mlrVST_AU` - Audio Unit (macOS only)

## Debugging with CLion

1. Set breakpoints in source files
2. Select **Debug** configuration (top-right)
3. Click Debug 🐛 icon
4. Run your DAW/plugin host under the debugger and load the plugin

## CLion Indexing Issues

If CLion shows lots of red underlines but builds fine:

1. **File → Invalidate Caches / Restart**
2. Select **Invalidate and Restart**
3. Wait for re-indexing to complete

## Platform-Specific Notes

### macOS
- Make sure Xcode Command Line Tools are installed
- CLion should auto-detect Xcode toolchain
- For code signing, edit CMakeLists.txt

### Linux
- Install all dependencies first:
  ```bash
  sudo apt-get install build-essential cmake libasound2-dev \
      libx11-dev libfreetype6-dev libjack-jackd2-dev
  ```
- CLion should auto-detect system GCC/Clang

### Windows
- **Visual Studio** method (recommended):
  - Install VS 2019+ with C++ workload
  - CLion will auto-detect
  
- **MinGW** method:
  - Install MSYS2
  - Add MinGW bin to PATH
  - Configure in CLion toolchains

## Alternative: Use Makefile Instead

If CMake in CLion is problematic, you can use the Makefile:

1. Open terminal in CLion (**View → Tool Windows → Terminal**)
2. Run:
   ```bash
   make
   ```
3. Build artifacts will be in `Build/` directory

## Verification

After successful build, check:
```
Build/mlrVST_artefacts/Debug/VST3/mlrVST.vst3
Build/mlrVST_artefacts/Debug/AU/mlrVST.component   # macOS
```

## Getting Help

If you still have issues:

1. Check **Tools → CMake → CMake** output window for errors
2. Verify JUCE version: Should be 8.0.4+
3. Check compiler versions:
   - GCC 9+
   - Clang 10+
   - MSVC 2019+
4. Try command-line build first:
   ```bash
   mkdir Build && cd Build
   cmake .. -DCMAKE_BUILD_TYPE=Debug
   cmake --build .
   ```

## Recommended CLion Settings

For best experience:

1. **Settings → Editor → Code Style → C/C++**
   - Set to match JUCE style

2. **Settings → Build, Execution, Deployment → CMake**
   - Enable: **Build directory**: `cmake-build-debug`
   - Build type: Debug (for development)

3. **Settings → Build, Execution, Deployment → CMake**
   - Generation path: Use default
   - Build options: `-j 8` (parallel build)

That's it! You should now be able to build mlrVST in CLion successfully.
