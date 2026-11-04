# Trying @YOKE_PROGRAM@

This directory is an application project that uses the Plugin in `..`: `yoke-compose new plugin` made
it with `init` and took the Plugin into it with `add plugin .. --yes`. Nothing here is the Plugin, and
taking the Plugin into another application never takes this directory.

After a change to the Plugin, take it again, from this directory:

    yoke-compose remove units.@YOKE_PROGRAM@
    rm components/plugins/@YOKE_PROGRAM@ manifests/@YOKE_ID@.yaml
    yoke-compose add plugin ..

then build the application and run it on a local Core.
