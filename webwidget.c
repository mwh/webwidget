// webwidget - display a web page like a desktop widget
// Copyright 2013 Michael Homer.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#define _POSIX_C_SOURCE 200809L // getline, fdopen
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gunixinputstream.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <webkit2/webkit2.h>

#define VERSION "0.3"

char *progname;
GtkWidget *window;
GtkWidget *webkit;

char *background_path;
int background_count;
char background_setter[2048];
int background_x, background_y;
time_t background_mtime;

char *uri = "webwidget:";

void do_quit(GtkWidget *widget, gpointer data) {
    exit(0);
}

// Executes the desktop background setter.
void setbg() {
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(webkit), background_setter, NULL, NULL, NULL);
}

// Constructs the JavaScript to set the desktop background.
void setupbg() {
    struct stat st;
    lstat(background_path, &st);
    background_mtime = st.st_mtime;
    background_count++;
    // Using an img this way means the file is pre-loaded when we switch
    // to the new background.
    sprintf(background_setter, 
             "var img = document.createElement('img'); img.onload = function() {document.body.style.background = 'url(desktop-background:///%i) fixed -%ipx -%ipx';}; img.onerror = img.onload; img.src = 'desktop-background:///%i';", background_count, background_x, background_y, background_count);
}

// If the desktop background file has changed we need to update the widget.
gboolean timeout(gpointer user_data) {
    struct stat st;
    lstat(background_path, &st);
    if (st.st_mtime != background_mtime) {
        setupbg();
        setbg();
    }
    return TRUE;
}

// When the window moves, we may need to recalculate the JavaScript.
gboolean window_configure(GtkWindow *window, GdkEvent *event, gpointer data) {
    background_x = event->configure.x;
    background_y = event->configure.y;
    if (!background_path)
        return false;
    setupbg();
    setbg();
    return false;
}

// Processes the desktop-background:// URI scheme.
void bg_scheme_callback(WebKitURISchemeRequest *request, gpointer user_data) {
    GInputStream *stream;
    int fd = g_open(background_path, O_RDONLY, 0);
    stream = g_unix_input_stream_new(fd, true);
    webkit_uri_scheme_request_finish(request, stream, -1, NULL);
    g_object_unref(stream);
}

#define HEXDEC(c) ((c >= '0' && c <= '9') ? c - '0' : ((c >= 'a' && c <= 'f') ? 10 + c - 'a' : c))
void urldecode(char *dest, const char *src) {
    while (*src != 0) {
        if (*src == '%') {
            src++;
            char c = 16 * HEXDEC(*src);
            src++;
            c += HEXDEC(*src);
            *dest = c;
        } else 
            *dest = *src;
        dest++, src++;
    }
    *dest = 0;
}

// Processes the webwidget: URI scheme.
void webwidget_scheme_callback(WebKitURISchemeRequest *request,
        gpointer user_data) {
    GInputStream *stream;
    const gchar *path = webkit_uri_scheme_request_get_path(request);
    stream = g_memory_input_stream_new_from_data(
            "<html>"
            " <head>"
            "  <style type=\"text/css\">"
            "   html, body { margin: 0; padding: 0; }"
            "  </style>"
            " <body>"
            , -1, NULL);
    if (strlen(path)) {
        // Must be heap-allocated because it is used later on
        char * buf = malloc(strlen(path) + 1);
        urldecode(buf, path);
        g_memory_input_stream_add_data(G_MEMORY_INPUT_STREAM(stream),
                buf, -1, free);
    } else {
        g_memory_input_stream_add_data(G_MEMORY_INPUT_STREAM(stream),
                "<h1>Web Widget</h1>Pass in a URL to get started."
                " (Press escape to close).", -1, NULL);
    }
    g_memory_input_stream_add_data(G_MEMORY_INPUT_STREAM(stream),
            "</body></html>", -1, NULL);
    webkit_uri_scheme_request_finish(request, stream, -1, "text/html");
    g_object_unref(stream);
}

// Re-run the background setter when the page loads.
void load_changed(WebKitWebView  *web_view,
        WebKitLoadEvent load_event, gpointer user_data) {
    if (load_event == WEBKIT_LOAD_FINISHED)
        setbg();
}

int help() {
    printf("Usage: %s [options...] URL\n", progname);
    puts("Display a web page like a desktop widget.");
    puts("");
    puts("  --desktop-background PATH  Show the right part of PATH in "
            "the background");
    puts("  --width WIDTH              Make the widget WIDTH pixels wide");
    puts("  --height HEIGHT            Make the widget HEIGHT pixels high");
    puts("  --help                     Display this help and exit");
    puts("  --version                  Print version information and exit");
    puts("");
    puts("webwidget will load a URL in a small borderless window.");
    puts("The window can be closed with the Escape key.");
    return 0;
}

int version() {
    puts("webwidget " VERSION);
    puts("Copyright (C) 2013 Michael Homer");
    puts("This program comes with ABSOLUTELY NO WARRANTY.");
    puts("See the source for copying conditions.");
    return 0;
}

int main (int argc, char **argv) {
    progname = argv[0];
    GtkAccelGroup *accelgroup;
    GClosure *closure;

    int width = 250;
    int height = 120;
    for (int i=1; i<argc; i++) {
        if (strcmp(argv[i], "--desktop-background") == 0)
            background_path = argv[++i];
        else if (strcmp(argv[i], "--width") == 0)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--version") == 0)
            exit(version());
        else if (strcmp(argv[i], "--help") == 0)
            exit(help());
        else
            uri = argv[i];
    }

    gtk_init(&argc, &argv);
 
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    closure = g_cclosure_new(G_CALLBACK(do_quit), NULL, NULL);
    accelgroup = gtk_accel_group_new();
    gtk_accel_group_connect(accelgroup, GDK_KEY_Escape, 0, 0, closure);
    gtk_window_add_accel_group(GTK_WINDOW(window), accelgroup);
 
    gtk_window_set_title(GTK_WINDOW(window), "Web Widget");
 
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    // Detect window movement
    g_signal_connect(G_OBJECT(window), "configure-event", G_CALLBACK(window_configure), NULL);
    
    webkit = webkit_web_view_new();
    gtk_container_add(GTK_CONTAINER(window), webkit);

    // Triggers when page-load completed
    g_signal_connect(webkit, "load-changed", G_CALLBACK(load_changed), NULL);

    // Create desktop-background:// and webwidget: URI schemes
    WebKitWebContext *context = webkit_web_view_get_context(
            WEBKIT_WEB_VIEW(webkit));
    webkit_web_context_register_uri_scheme(context, "desktop-background",
            bg_scheme_callback, NULL, NULL);
    webkit_web_context_register_uri_scheme(context, "webwidget",
            webwidget_scheme_callback, NULL, NULL);
    webkit_web_context_set_cache_model(context,
            WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    webkit_web_view_load_uri (WEBKIT_WEB_VIEW (webkit), uri);

    // Set up widget window
    gtk_widget_set_size_request (GTK_WIDGET(window), width, height);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
 
    gtk_widget_show_all(window);
 
    // We check whether the desktop background has changed every 5 seconds
    if (background_path)
        g_timeout_add_seconds(5, timeout, NULL);

    gtk_main();
 
    return 0;
}
