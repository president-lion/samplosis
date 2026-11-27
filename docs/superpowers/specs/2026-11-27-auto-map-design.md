# Auto-mapping samples by file name (short design)

**Intent.** Adding a multisampled instrument's files (`Ab3P.wav`, `Ab3MF.wav`, `C4P.wav`, ...) should give a playable
instrument at once: each sample on its note, loudness layers on one note splitting the velocity range, no hand
setting of Start key, End key and Velocity for each.

**Today.** Every added sample gets every key and velocity 1..127. A name only sets the root, and only when a whole
word is a note (`Guitar C3.wav`); `Ab3P` isn't read, so such files fall back to pitch detection.

**Approaches.** (a) Map on every add that brings in named samples, remapping the whole list (picked). (b) A separate
"Map by name" button (more steps, nothing automatic: not what was asked). (c) Map only the newly added samples
(adding a folder in pieces would leave gaps). Picked: (a).

**Design (approved).**
1. Names: a word that is a note followed by letters that are a dynamic counts (`Ab3P`, `C4mf`), as does a note word
   followed by a dynamic word (`Ab3 MF`, `C4_mf`). Dynamics, case-insensitive: ppp pp p mp mf f ff fff. The root
   comes from such names too.
2. Keys: the distinct notes sorted; each covers from just above halfway to the note below to halfway to the note
   above; the lowest reaches C-1, the highest G9.
3. Velocity: the samples on one note sorted softest first (no dynamic ranks as mf; ties keep list order) split 1..127 evenly: layer k of L gets 1 + k*127/L .. (k+1)*127/L (2 layers: 1-63, 64-127).
4. When: an Add samples (or a multi-file load) whose new files include at least one with a note in its name. Every
   sample in the list with a note in its name is remapped; ones without keep their ranges. Replacing one sample
   doesn't map.
5. Told: the status after the load ends ", mapped to N notes" and, with layers, " in up to L velocity layers"
   ("Ready: 11 samples, mapped to 6 notes in up to 2 velocity layers").

**Non-goals.** Round robins, articulations, reading notes as MIDI numbers in names.

**Builder's calls.** The parser lives in notes.c (`smp_dynamic_from_filename`), the mapping in multi.c
(`smp_multi_map_by_name`), the call in plugin.c's take_samples.
