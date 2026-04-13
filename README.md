# Description
OpenCDU is an open-source CDU (Control Display Unit) peripheral for Microsoft Flight Simulator. It runs on a Raspberry Pi Zero 2 W connecteed via USB Ethernet Gadget or SSH.

## Software
OpenCDU is synced to Microsoft Flight Simulator's SimConnect API, which externally sends variables from the active flight in the simulator to whatever is requesting the API.
The screen data is not sent as a full window, like many hardware peripherals do, but rather gets the individual character values and tags (color, size, etc) and reconstucts
it on the Pi.
The Pi runs a C++ script using SDL2. It receives JSON packets from the host C# script (unfinished as of 4/13/2026) over SSH or USB Ethernet Gadget then displays it on HDMI.

## Hardware
OpenCDU uses a custom PCB for the buttons (FSMIJ63AA04) which are backlit and synced to either the brightness dimmer potentiometer or the SimConnect data. The screen is a ZJ050NA-08C which gets converted to RGB 50pin from HDMI (Unconfirmed if this display works yet).
Since the Pi's GPIO is limited to far less than the existing buttons, there is an I2C expander which permits 32 pins, and the button signals are arranged in a matrix split by rows and columns to send signal data.


# Installation
To run the software, you need to compile it on the Pi using ARM32 GCC, then run the produced /main folder. You can also compile it on Windows with GNU Toolchain (I sent the source files to the Pi over SSH then compiled it locally. Always works.)

[TODO: actually fix the c# file it doesnt wanna compile] You need to then compile the C# host file and configure it for either PMDG or FBW using the aircraft string in program.cs, then run with `dotnet run`.


# Expected result

You should get a near exact recreation of your aircraft's CDU in the Pi's video output. 



The `client` folder is from my local opencdu-client repo which i merged into this one.
