# Rules for working in this repository

These rules are absolute. They apply to every file, every commit, and every generated string, including code comments, serial log lines, status strings, and the web page. Where any other document disagrees with this one, this one wins.

## Attribution

- Never add Claude, any AI assistant, or any AI tool as an author, co-author, contributor, or credit of any kind. Not in commit messages, not in Co-Authored-By trailers, not in comments, not in documentation.
- Commit messages carry no trailers, no badges, no generated-with lines. Subject in the imperative, lowercase, under 60 characters. Body only when the change needs explaining, wrapped at 72 columns.
- Author is Moheeb Zara <hackbuildvideo@gmail.com>.

## Language

- No emojis, anywhere.
- No em dashes or en dashes. Use commas, periods, or the word "to" for ranges.
- No "it is not x, it is y" construction or its variants.
- No closing line that restates the point.
- No exclamation marks in prose, logs, or UI copy.
- No marketing adjectives: seamless, powerful, robust, comprehensive, beautiful, elegant, and their kin. Say what it does.
- No filler openers: "Let's", "Simply", "Just", "Note that".
- Log lines and UI copy are lowercase and terse, and name the failure: "no WiFi set, send: /wifi <ssid> <password>" beats "error".
- Never claim a capability the hardware does not have.

## Firmware

- No heap allocation per message. Buffers are sized at compile time or allocated once at boot.
- Every blocking network call has a deadline.
- Only the player task drives the servo. Only the net task touches sockets.
- The jaw never leaves the calibrated range outside the explicit calibration commands.
- `pio test -e native` and `pio run -e buck -e heatsync` pass before a commit.
