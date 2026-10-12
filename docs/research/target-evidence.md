# Checking a real build

For when a Series homebrew build is available. The point is to write down what
the file actually contains before implementing anything for it.

1. Note where the build comes from: source revision, project configuration,
   GDK version.
2. Hash the executable and the staged `MicrosoftGame.config`.
3. Run:

   ```
   xscarlet inspect title.exe
   xscarlet inspect-config MicrosoftGame.config
   xscarlet assess title.exe --config MicrosoftGame.config --entry title.exe
   xscarlet bind title.exe
   ```

4. Compare with a desktop or Xbox One build of the same source if there is one.
5. Write down what the title does on a console so there is something to
   compare against.

Do not commit the binaries or any SDK files, only the notes.

## About the config check

`assess --config` compares the `TargetDeviceFamily` and `Architecture` that
the config declares for an executable with what the PE says. The valid values
are documented in the
[Executable element reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/microsoftgameconfig/elements/microsoftgameconfig-element-executable).
A mismatch shows up as a conflict in the report, not as an error.

None of this proves a file is a genuine console build. Import names and config
values are easy to fake.
