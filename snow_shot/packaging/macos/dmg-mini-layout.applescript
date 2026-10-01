-- CPack passes the mounted volume name as the sole argument.
on run arguments
    set volumeName to item 1 of arguments
    tell application "Finder"
        tell disk volumeName
            open
            set current view of container window to icon view
            set toolbar visible of container window to false
            set statusbar visible of container window to false
            set bounds of container window to {120, 120, 900, 702}
            set theOptions to icon view options of container window
            set arrangement of theOptions to not arranged
            set icon size of theOptions to 96
            set text size of theOptions to 14
            set background picture of theOptions to file ".background:background.png"
            set position of item "Snow Shot Mini.app" to {210, 224}
            set position of item "Applications" to {570, 224}
            -- Keep CPack's hidden assets outside the visible window even when
            -- the packager's Finder is configured to show hidden files.
            set position of item ".background" to {900, 100}
            update without registering applications
            close
            open
            delay 2
            close
        end tell
    end tell
end run
