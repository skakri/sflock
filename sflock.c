/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>

static void
die(const char *errstr, ...) {
    va_list ap;

    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static const char *
get_password() { /* only run as root */
    const char *rval;
    struct passwd *pw;

    if (geteuid() != 0)
        die("sflock: cannot retrieve password entry (make sure to suid sflock)\n");
    pw = getpwuid(getuid());
    endpwent();
    rval = pw->pw_passwd;

#if HAVE_SHADOW_H
    {
        struct spwd *sp;
        sp = getspnam(getenv("USER"));
        endspent();
        rval = sp->sp_pwdp;
    }
#endif

    /* drop privileges temporarily */
    if (setreuid(0, pw->pw_uid) == -1)
        die("sflock: cannot drop privileges\n");
    return rval;
}

int
main(int argc, char **argv) {
    char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
    char buf[32], passwd[256];
    int num, screen, width, height, update, sleepmode, term, pid;

    const char *pws;
    unsigned int len;
    Bool running = True;
    Cursor invisible;
    Display *dpy;
    KeySym ksym;
    Pixmap pmap;
    Window root, w;
    XColor black, red, dummy;
    XEvent ev;
    XSetWindowAttributes wa;
    GC gc;
    XGCValues values;

    /* disable tty switching */
    if ((term = open("/dev/console", O_RDWR)) == -1) {
        perror("error opening console");
    }

    if ((ioctl(term, VT_LOCKSWITCH)) == -1) {
        perror("error locking console");
    }

    /* deamonize */
    pid = fork();
    if (pid < 0)
        die("Could not fork sflock.");
    if (pid > 0)
        exit(0); // exit parent 

    pws = get_password();

    if (!(dpy = XOpenDisplay(0)))
        die("sflock: cannot open dpy\n");

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    width = DisplayWidth(dpy, screen);
    height = DisplayHeight(dpy, screen);

    wa.override_redirect = 1;
    wa.background_pixel = XBlackPixel(dpy, screen);
    w = XCreateWindow(
        dpy, root, 0, 0, width, height,
        0, DefaultDepth(dpy, screen), CopyFromParent,
        DefaultVisual(dpy, screen),
        CWOverrideRedirect | CWBackPixel, &wa);

    XAllocNamedColor(
        dpy, DefaultColormap(dpy, screen), "orange red", &red, &dummy);
    XAllocNamedColor(
        dpy, DefaultColormap(dpy, screen), "black", &black, &dummy);
    pmap = XCreateBitmapFromData(dpy, w, curs, 8, 8);
    invisible = XCreatePixmapCursor(dpy, pmap, pmap, &black, &black, 0, 0);
    XDefineCursor(dpy, w, invisible);
    XMapRaised(dpy, w);

    gc = XCreateGC(dpy, w, 0, &values);
    XSetForeground(dpy, gc, XWhitePixel(dpy, screen));

    for (len = 1000; len; len--) {
        if (XGrabPointer(
            dpy, root, False,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
            GrabModeAsync, GrabModeAsync, None, invisible,
            CurrentTime) == GrabSuccess)
            break;
        usleep(1000);
    }
    if ((running = running && (len > 0))) {
        for (len = 1000; len; len--) {
            if (XGrabKeyboard(
                dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
                == GrabSuccess)
                break;
            usleep(1000);
        }
        running = (len > 0);
    }

    len = 0;
    XSync(dpy, False);
    update = True;
    sleepmode = False;

    /* main event loop */
    while (running && !XNextEvent(dpy, &ev)) {
        if (sleepmode) {
            DPMSEnable(dpy);
            DPMSForceLevel(dpy, DPMSModeOff);
            XFlush(dpy);
        }

        if (update) {
            XClearWindow(dpy, w);
            update = False;
        }

        if (ev.type == MotionNotify) {
            sleepmode = False;
        }

        if (ev.type == KeyPress) {
            sleepmode = False;

            buf[0] = 0;
            num = XLookupString(&ev.xkey, buf, sizeof buf, &ksym, 0);
            if (IsKeypadKey(ksym)) {
                if (ksym == XK_KP_Enter)
                    ksym = XK_Return;
                else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
                    ksym = (ksym - XK_KP_0) + XK_0;
            }
            if (IsFunctionKey(ksym) || IsKeypadKey(ksym)
                || IsMiscFunctionKey(ksym) || IsPFKey(ksym)
                || IsPrivateKeypadKey(ksym))
                continue;

            switch (ksym) {
                case XK_Return:
                    passwd[len] = 0;
                    running = strcmp(crypt(passwd, pws), pws);
                    if (running != 0)
                        // change background on wrong password
                        XSetWindowBackground(dpy, w, red.pixel);
                    len = 0;
                    break;
                case XK_Escape:
                    len = 0;

                    if (DPMSCapable(dpy)) {
                        sleepmode = True;
                    }

                    break;
                case XK_BackSpace:
                    if (len)
                        --len;
                    break;
                default:
                    if (num && !iscntrl((int) buf[0]) &&
                        (len + num < sizeof passwd)) {
                        memcpy(passwd + len, buf, num);
                        len += num;
                    }

                    break;
            }

            update = True; // show changes
        }
    }

    /* free and unlock */
    setreuid(geteuid(), 0);
    if ((ioctl(term, VT_UNLOCKSWITCH)) == -1) {
        perror("error unlocking console");
    }

    close(term);
    setuid(getuid()); // drop rights permanently

    XUngrabPointer(dpy, CurrentTime);
    XFreePixmap(dpy, pmap);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, w);
    XCloseDisplay(dpy);
    return 0;
}
