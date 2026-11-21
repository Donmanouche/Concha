// Concha's .timeline(n) (Strudel's core/impure.mjs, AGPL-3.0, Strudel contributors): a pattern that
// starts from its beginning, aligned on a cycle, the first time a timeline number is played, and
// keeps that start while the number stays.
//
// Differences with Strudel's: the offsets live here, so that Concha can hand them from one JS engine
// to the other at each code swap (getTimelines / setTimelines, PatternWorker) and clear them when
// the music stops (resetTimelines); only the music's queries (controls.cyclist) record an offset,
// not the oscilloscope's queries ahead or the sample warm-up.
import { register, reify, Pattern } from '@strudel/core';

let timelines = {};

export const getTimelines = () => JSON.stringify(timelines);
export const setTimelines = (json) => {
  timelines = JSON.parse(json || '{}');
};
export const resetTimelines = () => {
  timelines = {};
};

export const timeline = register(
  'timeline',
  function (tpat, pat) {
    tpat = reify(tpat);
    const f = function (state) {
      const scheduler = !!state.controls.cyclist;
      const result = [];
      for (const timehap of tpat.query(state)) {
        const tlid = timehap.value;
        let offset;
        if (tlid === 0) {
          offset = 0;
        } else if (tlid in timelines) {
          offset = timelines[tlid];
        } else {
          const timearc = timehap.wholeOrPart();
          // first seen over halfway into its cycle: starts at the next one (cueing up)
          offset = !scheduler || state.span.begin.lt(timearc.midpoint()) ? timearc.begin : timearc.end;
          offset = offset.valueOf();
        }
        if (scheduler) {
          timelines[tlid] = offset;
          if (tlid !== 0) delete timelines[-tlid];
        }
        result.push(
          ...pat
            .late(offset)
            .query(state.setSpan(timehap.part))
            .map((h) => h.setContext(h.combineContext(timehap))),
        );
      }
      return result;
    };
    return new Pattern(f, pat._steps);
  },
  false,
);
