package com.psprecomp.bbs;

import android.content.Context;

import java.io.File;

/**
 * Story slots (2026-11-20): one automatic save state per character, so switching between Terra's, Ventus's and
 * Aqua's stories is one tap instead of a trip through the title screen. host/main.cpp reports who is playing
 * (nativeCharacter: 1 Ventus, 2 Aqua, 3 Terra, 0 unknown); BbsActivity saves that character's slot when the app goes
 * to the background, on quit, and with every manual save, and loads another character's slot from "Switch story".
 */
final class StorySlots {
    static final String[] NAMES = {"", "Ventus", "Aqua", "Terra"};

    private StorySlots() {}

    static boolean valid(int character) { return character >= 1 && character <= 3; }

    static File file(Context c, int character) {
        File dir = new File(c.getFilesDir(), "states");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return new File(dir, "story-" + NAMES[character].toLowerCase(java.util.Locale.ROOT) + ".bbsstate");
    }
}
