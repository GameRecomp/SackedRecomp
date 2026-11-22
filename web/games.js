// SPDX-License-Identifier: MIT
// The games the converter makes builds of, as the page, the worker and the player name them. The
// id is the converter's (games/<id>/game.toml) and the directory the browser stores the game in.
// `source` is what the user brings: the GOG installer (with its `slice`, the -1.bin file beside it).
export const GAMES = {
  heroes4: {
    id: 'heroes4',
    name: 'Heroes IV',
    full: 'Heroes of Might and Magic IV',
    stem: 'Heroes4',
    installer: 'setup_heroes_of_might_and_magic_4_complete_3.0_(german)_(25023).exe',
    // The installer keeps its data in a second file beside it, which is needed too.
    slice: 'setup_heroes_of_might_and_magic_4_complete_3.0_(german)_(25023)-1.bin',
    installerSize: 'about 1 GiB with its .bin',
    size: 'about 1.1 GiB',
    web: 'heroes4',
    icon: 'heroes4.svg',
  },
};


/* The game's icon on the page: the tab's and every img.game-icon. */
export function showIcon(game) {
  for (const l of document.querySelectorAll('link[rel=icon]')) l.href = game.icon;
  for (const i of document.querySelectorAll('img.game-icon')) i.src = game.icon;
}

/* The game a page is for: ?game=<id> (or #<id>), else the default. */
export function gameOf(location) {
  const p = new URLSearchParams(location.search).get('game') || location.hash.replace(/^#/, '');
  return GAMES[p] || GAMES.heroes4;
}
