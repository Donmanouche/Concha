# Phase 0 — bilan (16 novembre 2026)

Objectif : lever les risques du plan ([PLAN.md](PLAN.md)) avant d'écrire le moteur.

## Résultats

| Risque | Test | Résultat |
|---|---|---|
| Strudel tourne-t-il hors du navigateur, sans modification ? | `js/spike/tunes-entry.mjs` : le runtime de test de Strudel, sans `@strudel/webaudio`, mis en bundle par `js/build.mjs` (esbuild). Il est comparé aux snapshots officiels (`test/__snapshots__/tunes.test.mjs.snap`). | **31/31 morceaux identiques dans node.** |
| QuickJS-ng embarqué est-il fidèle ? | `spikes/qjs_tunes.cpp` charge le même bundle dans QuickJS-ng 0.18 (bibliothèque statique, API C). | **30/31 identiques.** Le dernier ne diffère qu'au 15ᵉ chiffre d'un flottant (`cutoff 2727.530217714818` contre `…817`, `Math.sin` de QuickJS contre celui de V8). C'est inaudible. |
| QuickJS est-il assez rapide ? | Temps de `evaluate` + `queryArc`, rapporté au nombre de cycles. | Chargement du bundle : 140 ms par instance. Tas : 12 Mo. **20 ms par cycle en moyenne**, 150 ms au pire (`festivalOfFingers`, 290 haps par cycle, transpilation comprise). Node est environ 9 fois plus rapide, mais pour un cycle de 2 s (0,5 cps), 150 ms font environ 7 % d'un cœur, sur le fil pattern et jamais sur le fil audio. **V8 n'est pas nécessaire.** |
| Le squelette JUCE 8 + CLAP produit-il les bons formats ? | `Concha.clap`, `Concha.vst3` et Standalone se compilent (JUCE 8.0.15 + clap-juce-extensions). | Carla (`carla-discovery-native`) voit le VST3 : **32 sorties (16 stéréo), entrée et sortie MIDI.** Carla 2.5.9 ne lit pas le CLAP. **Bitwig : test validé par l'utilisateur le 16/11/2026** (copie dans `~/.clap` et `~/.vst3`). |
| Peut-on faire tourner scsynth dans le plugin ? | `cmake/ScSynth.cmake` compile scsynth 3.13.0 depuis les sources, avec un driver « hôte » (`src/sc/SC_HostDriver.cpp`, API C `src/sc/ConchaSc.h`). L'horloge OSC est comptée en samples. | **Deux Worlds dans le même processus, pilotés depuis un seul fil : chaque note démarre au sample prévu** (+1, le sinus vaut 0 au premier sample), avec sa propre fréquence, sans fuite d'un World à l'autre. 1,5 s de rendu pour deux instances en 1,5 ms. Aucune modification des sources de SuperCollider (API audio n° 7). |
| Le paquet `dough` de Strudel peut-il servir de base au DSP ? | Lecture de `@strudel/dough` et de `dough-synth` 0.4.1 (npm). | **Pas comme base : on réécrit superdough en C++ (décision du 16/11/2026).** `dough.h` est un moteur C en un seul fichier (3 900 lignes, AGPL, de Felix Roos). Il a été écrit pour tourner en natif et en WASM. Il contient oscillateurs, ADSR, biquads, FM/AM/RM, phaser, flanger, chorus, distorsion, crush, coarse, delay, réverb Dattorro par orbit, PCM et polyphonie par groupe. Mais c'est une intégration optionnelle (`.dough()`), encore en cours d'écriture, et non le son par défaut de strudel.cc. Il lui manque supersaw, wavetables, zzfx, bytebeat, soundfonts, vowel, ladder, djf, compresseur, tremolo, `duckorbit`, transient, time-stretch et les LFO. Sa réverb Dattorro sonne autrement que la convolution de superdough. On n'en reprend que des morceaux : oscillateurs, voix, PCM, chorus, flanger. |

## Ce que les tests ont appris (à garder pour la suite)

- **`OffsetOut` est obligatoire** pour qu'une SynthDef démarre au sample près. Avec `Out`, la note démarre au début du bloc de 64 (31 samples d'avance dans le test). Les SynthDefs de SuperDirt utilisent déjà `OffsetOut`.
- **Il ne faut pas charger `UIUGens.so`** (fourni par le système) dans le plugin. Il lance un fil pour la souris, qui fait planter le processus à sa sortie. Concha compilera son propre jeu de UGens (de base et sc3-plugins), sans UIUGens.
- scsynth exige une fonction de réponse, même vide : `World_SendPacket` avec `nullptr` plante sur `/done`.
- **`dough` a un état global** (`engine`, `schedule`, `frame`) et mélange toutes les orbits dans une seule sortie stéréo. Si on garde un mode `.dough()`, il faudra un contexte par instance et une sortie par orbit.
- **superdough ne peut pas tourner tel quel** : il s'appuie sur WebAudio du navigateur et sur 17 worklets JavaScript qui devraient tourner dans QuickJS sur le fil audio (trop lent, et le ramasse-miettes provoque des craquements).
- `evalScope` est asynchrone : il faut l'attendre avant la première évaluation.
- Le projet est sur **Ubuntu 24.04**, pas 22.04.
- Les sous-modules git récupérés en `--shallow-submodules` arrivent parfois vides (clap, clap-helpers, nova-tt). Il faut vérifier puis lancer `git submodule update --init`.

## Le test dans Bitwig (validé le 16/11/2026)

Le « sync probe » est actif par défaut :
- clic sur la sortie Main à chaque temps (plus aigu sur le premier temps de la mesure) ;
- une note par sortie Orbit 1 à 15 sur chaque premier temps, chacune à une hauteur différente ;
- en sortie MIDI : note C4 à chaque temps (C5 sur le premier), CC 74 qui monte par paliers, Program Change (mesure % 8) à chaque mesure.

À vérifier dans Bitwig, de préférence en CLAP :
1. Le plugin se charge et la fenêtre s'ouvre (« OpenGL renderer active » en bas).
2. Les clics tombent sur la grille, y compris en boucle et après un saut de position.
3. Les 15 sorties Orbit apparaissent comme pistes séparées (Bitwig : « Show Chain Outputs »).
4. Un clip MIDI enregistré depuis Concha contient les notes, le CC 74 et les Program Change aux bons endroits.
5. Free run : avec le transport à l'arrêt, les clics continuent.
6. La même chose en VST3, surtout le CC 74 et les Program Change.

## Versions figées

- Strudel `cac73dc5` (codeberg main, 15/11/2026), dans `third_party/strudel`.
- QuickJS-ng `v0.18.0`, dans `third_party/quickjs-ng`.
- JUCE `8.0.15`, clap-juce-extensions `6bde108` (`../deps`).
- SuperCollider `Version-3.13.0` (`../deps`). Les UGens du test viennent de `/usr/lib/SuperCollider/plugins`.
- dough-synth 0.4.1 (version utilisée par `@strudel/dough`), examiné dans le scratchpad. Il n'est pas encore intégré.
- npm (`js/`) : esbuild 0.25.11, fraction.js 5.2.1, acorn 8.14.0, escodegen 2.1.0, estree-walker 3.0.3, @tonaljs/tonal 4.10.0, chord-voicings 0.0.1.

## Commandes

```
cmake -S . -B build -G Ninja && ninja -C build Concha_CLAP Concha_VST3 Concha_Standalone
node js/build.mjs js/spike/tunes-entry.mjs build/js/tunes.js
node js/spike/compare.mjs --bundle build/js/tunes.js                 # node
build/spikes/spike_qjs_tunes build/js/tunes.js build/js/qjs.json && node js/spike/compare.mjs --json build/js/qjs.json
QT_QPA_PLATFORM=offscreen sclang spikes/sc/ping.scd build/spikes/synthdefs
build/spikes/spike_sc_two_worlds build/spikes/synthdefs/concha_ping.scsyndef
```
