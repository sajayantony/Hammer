========================================================================
    CONSOLE APPLICATION : HttpTest Project Overview
========================================================================

AppWizard has created this HttpTest application for you.

This file contains a summary of what you will find in each of the files that
make up your HttpTest application.


HttpTest.vcxproj
    This is the main project file for VC++ projects generated using an Application Wizard.
    It contains information about the version of Visual C++ that generated the file, and
    information about the platforms, configurations, and project features selected with the
    Application Wizard.

HttpTest.vcxproj.filters
    This is the filters file for VC++ projects generated using an Application Wizard. 
    It contains information about the association between the files in your project 
    and the filters. This association is used in the IDE to show grouping of files with
    similar extensions under a specific node (for e.g. ".cpp" files are associated with the
    "Source Files" filter).

HttpTest.cpp
    This is the main application source file.

/////////////////////////////////////////////////////////////////////////////
Other standard files:

StdAfx.h, StdAfx.cpp
    These files are used to build a precompiled header (PCH) file
    named HttpTest.pch and a precompiled types file named StdAfx.obj.

/////////////////////////////////////////////////////////////////////////////
Other notes:

AppWizard uses "TODO:" comments to indicate parts of the source code you
should add to or customize.

/////////////////////////////////////////////////////////////////////////////


start cmd /k wcctl.exe -t client.ubr -f settings.ubr -s %COMPUTERNAME% -v 5 -c 1 -o output.xml -x
wcclient.exe %COMPUTERNAME%
start output.xml