# Phase 1 — moteur de patterns sans interface (16 novembre 2026)

Objectif ([PLAN.md](PLAN.md)) : faire tourner le langage Strudel complet dans Concha, hors du plugin, et prouver qu'il donne les mêmes haps que Strudel.

## Ce qui existe maintenant

| Pièce | Fichier | Rôle |
|---|---|---|
| Runtime Strudel | `js/src/concha.mjs` | Charge les paquets Strudel en JavaScript pur : core, mini, transpiler, tonal, xen, edo, mondo, gamepad et les contrôles de dough. Utilise le `repl()` de Strudel lui-même, donc `$:`, labels, `_$:`, `S$:`, `all`, `each`, `hush`, `setcps` et `.cpm()` se comportent comme sur strudel.cc. Expose `concha.evaluate`, `concha.query`, `concha.showHaps`, `concha.setSlider`, `concha.log` et `concha.runSuite`. |
| Remplaçants du navigateur | `js/src/stubs.mjs`, liste générée `js/src/generated/browser-api.json` (par `js/tools/gen-browser-api.mjs`) | Définit tous les noms publics des paquets qui ont besoin d'un navigateur (draw, webaudio/superdough, codemirror, hydra, serial, soundfonts, csound, motion, mqtt, dough, midi, osc, tidal), soit 315 exports et 35 méthodes ou widgets. Les méthodes de sortie (`midi`, `osc`, `dough`, `csound`, `mqtt`, `serial`) sortent leurs haps avec leur propre genre (`kind`) au lieu du son par défaut, comme dans Strudel. `scope`, `fscope`, `tscope` et `spectrum` ajoutent `analyze`. Les widgets (`_pianoroll`, `_scope`…) sont enregistrés auprès du transpiler. `slider()` est fonctionnel. Le reste renvoie le pattern ou la valeur reçue. |
| Cales | `js/src/shims.mjs` | `document`, `window`, `navigator`, `performance` et timers. Les logs de Strudel sont récupérés. |
| Bundle | `cmake/StrudelBundle.cmake`, `js/build.mjs`, `tools/embed` | esbuild assemble le bundle (2,3 Mo), qui est ensuite compilé dans le binaire. node ne sert qu'à la compilation. |
| Moteur C++ | `src/pattern/PatternEngine.{h,cpp}` | QuickJS-ng. `evaluate(code)` renvoie ok ou une erreur, avec les emplacements de la mini-notation. `query(début, fin, cps)` renvoie les événements avec une attaque dans la fenêtre : genre, début et fin en cycles, paramètres (nombres, textes, objets en JSON) et emplacements dans le code. Délai maximal : 5 s pour l'évaluation, 1 s par requête, pour arrêter une boucle sans fin. Limite mémoire : 512 Mo. Logs disponibles par `takeLog()`. |
| Outil | `tools/concha_query` (`build/concha_query`) | Code → événements (`--cycles`, `--from`, `--cps`, `--show`), mesure de performances (`--bench`), suite de parité (`--suite`). |
| Tests | `tests/pattern_tests.cpp` (CTest, petit harnais maison `tests/check.h`) | 13 tests : valeurs et temps, fenêtre de requête, emplacements, labels, erreur de syntaxe, boucle sans fin, erreur pendant une requête, sortie `midi()`, objets, tonal et signaux, déterminisme, indépendance de deux moteurs. |

## Parité avec Strudel

Suite : `js/test/make-cases.mjs` reprend les **31 morceaux** de `test/tunes.test.mjs` et **515 exemples de la documentation** (`test/examples.test.mjs`), avec les haps attendus par les snapshots de Strudel. 11 snapshots d'exemples ne correspondent plus à aucun exemple dans les sources. Chaque cas tourne dans un runtime neuf, comme une page qu'on vient d'ouvrir : `setDefaultJoin` et `useRNG` changent un réglage global qui, sinon, se répercuterait sur les cas suivants.

| Où | Identiques | À 1e-9 près | Différences connues | Inexpliquées |
|---|---|---|---|---|
| node (V8) | 525 | 0 | 21 | **0** |
| QuickJS (moteur C++ réel, `concha_query --suite`) | 523 | 2 | 21 | **0** |

Les 21 différences connues (`js/test/known-differences.json`) viennent toutes du runtime de test de Strudel, pas de strudel.cc. Dans tous ces cas, Concha se comporte comme strudel.cc :
- ce runtime remplace par des fonctions vides `tune`, `filter`, `adsr` et `markcss`, alors que Concha les exécute vraiment ;
- il ne garde que le dernier `$:` ou pattern nommé, alors que le `repl` de strudel.cc les empile tous ;
- il n'a pas de `window`, donc `keyDown` et `whenKey` y échouent.

Les 2 différences à 1e-9 près portent sur le 15ᵉ chiffre d'un nombre à virgule (`Math.sin` de QuickJS contre celui de V8).

## Performances (QuickJS, une instance)

- Démarrage du moteur : environ 150 ms. Tas : environ 12 Mo.
- Requêtes de 1/16 de cycle, comme le fera le scheduler :

| Morceau | Coût moyen par cycle | Pire requête | node, pour comparaison |
|---|---|---|---|
| `festivalOfFingers` (le plus lourd) | 300 ms | 39 ms | 117 ms par cycle |
| `amensister` | 176 ms | 22 ms | |
| `meltingsubmarine` | 208 ms | 17 ms | |
| `caverave` | 100 ms | 12 ms | 38 ms par cycle |

- À 0,5 cps, `festivalOfFingers` occupe environ 15 % d'un cœur, sur le fil pattern. Le fil audio n'est pas concerné.
- **Évaluation** : quelques ms à quelques dizaines de ms en général, mais **1,7 s pour `caverave`** (50 ms dans node).

## À reprendre dans les phases suivantes

- **Phase 2, évaluation sans trou** : pendant une évaluation longue, un moteur unique ne peut pas répondre aux requêtes. Je propose **deux moteurs en alternance** : B évalue le nouveau code pendant que A continue de servir, puis on bascule. Si la mémoire devient un problème, on pourra aussi agrandir la fenêtre d'avance.
- Phase 2 : traduire les événements `kind: midi` en messages MIDI (logique de `@strudel/midi/midi.mjs`), et `sound` vers le scheduler audio.
- Les fonctions de dessin (`pianoroll`, `punchcard`…) renvoient pour l'instant le pattern sans rien changer. La phase 8 les implémentera.
- `@strudel/tidal` (traduction Haskell → JS) demande `web-tree-sitter` en WASM. C'est prévu en phase 7, avec un téléchargement à valider.
- Pistes d'optimisation si besoin : bytecode QuickJS précompilé pour le démarrage, et mesure de l'évaluation de `caverave` (probablement tonal ou les regex).

## Commandes

```
cmake -S . -B build -G Ninja && ninja -C build
(cd build && ctest)                                              # tests unitaires
echo 's("bd*2 sd").bank("tr909")' | build/concha_query --cycles 2
build/concha_query --bench mon-morceau.js
node js/test/make-cases.mjs build/js/cases.json                  # suite de parité
node js/test/check.mjs build/js/cases.json --bundle build/js/concha-strudel.js           # dans node, ~40 s
build/concha_query --suite build/js/cases.json build/js/qjs-suite.json && node js/test/check.mjs build/js/cases.json --json build/js/qjs-suite.json   # QuickJS, ~2 min
node js/tools/gen-browser-api.mjs > js/src/generated/browser-api.json   # après une mise à jour de Strudel
```
