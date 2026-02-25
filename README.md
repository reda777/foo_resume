# foo_resume

A lightweight foobar2000 component that automatically remembers and resumes the playback position for every track.

## Features
* Remembers the last playback position for every unique track in your library.
* Easily enabled or disabled via the advanced settings menu.
* Stores data in a simple text file within your foobar2000 profile folder.

## Installation
1. Download the latest `foo_resume.fb2k-component` from the [Releases](https://github.com/reda777/foo_resume/releases) page.
2. Install the component using one of the following methods:
   * **Double-click** the `foo_resume.fb2k-component` file.
   * **Drag and drop** the file into the **File -> Preferences -> Components** window.
   * Open **File -> Preferences -> Components**, click the **Install...** button, and select the file.

## Configuration
To enable or disable the component:
* Navigate to **File -> Preferences -> Advanced -> Tools -> foo_resume**.

## Build Instructions
Follow these steps to compile the component from the source.

### Prerequisites
* **Visual Studio 2022** (Community Edition or higher) with the "Desktop development with C++" workload.
* [**foobar2000 SDK**](https://www.foobar2000.org/SDK) (Version 2025-03-07 recommended).
* [**PowerShell 7.2**](https://github.com/PowerShell/PowerShell) or later.

### Setup
1. Clone this repository into your local development folder.
2. Download the foobar2000 SDK and extract the `foobar2000`, `pfc`, and `libPPUI` folders into a sibling directory named `sdk`.
3. Ensure your folder structure matches the following layout:
   ```text
   /parent-directory/
   ├── sdk/                <-- (Extracted SDK folders)
   │   ├── foobar2000/
   │   ├── pfc/
   │   └── libPPUI/
   ├── foo_resume/         <-- (This repository)
   └── out/                <-- (contains the .fb2k-component)
4. Open foo_resume.sln in Visual Studio.
5. Go to Build -> Batch Build, and check the Release configuration for both x64 and x86 (Win32)
6. Click Build. The finished component will be generated in the out/ folder.

## Credits 
Inspired by the project structure and build automation of [foo_midi](https://github.com/stuerp/foo_midi) by stuerp.
