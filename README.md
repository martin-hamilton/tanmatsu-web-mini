# Tanmatsu Web Mini

This is a very hacky attempt to port Mac Tjaap's [WHY2025 badge mini web browser](https://github.com/mactjaap/mini_browser) to run on the Tanmatsu. If you don't know what any of those things mean, this probably isn't the repo for you.

Right now there is no binary - you'll need to build from source using the ESP-IDF tools then use `badgelink` to copy `build/tanmatsu/application.bin` over to your Tanmatsu.

Just to set expectations, this is a web browser which only displays text - all the rest of the web page is discarded, so no images, CSS, JavaScript etc.

## What to expect

- On startup you should be taken to [info.cern.ch](https://info.cern.ch), the OG website.

- You should find that you can page backwards and forwards using the arrow keys.

- You should be able to use the TAB key to cycle through the URLs on the page, then press ENTER to pick one. Except right now there is no visual feedback to tell you which link you picked. Choose wisely!

- The SSL implementation doesn't agree with some websites, so you might find that you follow a link which doesn't work. Check the ESP-IDF monitor (`make monitor`) for diagnostics.

- Of the function keys, X quits the app and returns you to the Tanmatsu launcher, Triangle cycles through keyboard backlight brightness levels, Square does the same for the screen backlight, and Circle will open up a dialog box for you to enter the URL of a site you want to visit.

- When you enter a URL in the dialog box, the browser should add _https://_ if you didn't specify a URL scheme. Right now there isn't any feedback about what is going on, so the app might appear to hang while it is trying to fetch the URL.

- The app is a bit flaky and might crash or just stop responding. Again, check the output of `make monitor` in case there are some clues. Patches welcome :-)

## What's next?

There's a bit of work to do to add user settings and feedback, improve the visual presentation, make it more robust etc etc. This is very much a minimum viable (and sometimes not viable!) browser, so calibrate your expectations accordingly!

## Acknowledgements

This app borrows liberally from the Tanmatsu Launcher and the aforementioned WHY2025 mini browser, however any bugs and glitches you may encounter are entirely my own work.


