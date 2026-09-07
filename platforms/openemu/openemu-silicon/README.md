# OpenEmu-Silicon channel metadata

OpenEmu-Silicon takes ownership of every installed core's update channel. On
startup it replaces a non-canonical `SUFeedURL` with:

```text
https://raw.githubusercontent.com/OpenEmu-Silicon/OpenEmu-Silicon/main/Appcasts/<lowercase bundle-id suffix>.xml
```

For `org.gearsc3000.GearSC3000`, that URL ends in `gearsc3000.xml`. This is a
policy of the fork, not evidence of another core with the same name.

After creating an `openemu-vX.Y.Z` release in this repository, integration in
OpenEmu-Silicon requires a pull request there which:

1. adds the generated `build/gearsc3000.xml` as
   `Appcasts/gearsc3000.xml`;
2. inserts `oecores-entry.xml` inside the root `<cores>` element of
   `oecores.xml`;
3. signs the ZIP/appcast entry with OpenEmu-Silicon's Sparkle EdDSA key, using
   its `Scripts/update_core_appcast.py --sign-zip` release path.

The private Sparkle key belongs to OpenEmu-Silicon and must not be generated or
stored in this repository. Until that pull request is accepted, the fork will
rewrite the installed GearSC3000 feed to an URL which returns 404. The plugin
continues to run; only automatic updates through that fork are unavailable.
