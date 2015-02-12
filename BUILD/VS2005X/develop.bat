@echo off
@rem $Id: develop.bat 1.1 Thu, 02 Aug 2007 16:24:27 -0700 dickey $
@rem ensure that all IDE files are writable

attrib -r *.bat /s
attrib -r *.sln /s
attrib -r *.vcproj /s