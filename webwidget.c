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
char *background_tint;
bool adjust_text;

char *uri = "webwidget:";

bool allow_shell = 0;

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

void runshell(char *cmd) {
    if (!fork()) {
        execlp("sh", "sh", "-c", cmd, NULL);
    }
}

// Determines whether to navigate to a given URL; also used for
// shell: protocol.
gboolean handle_policy(WebKitWebView *web_view,
        WebKitNavigationPolicyDecision *policy_decision,
        WebKitPolicyDecisionType decision_type, gpointer user_data) {
    if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return false;
    if (!allow_shell)
        return false;
    WebKitURIRequest *request = webkit_navigation_policy_decision_get_request(policy_decision);
    const gchar *uri = webkit_uri_request_get_uri(request);
    if (uri[0] == 's' && uri[1] == 'h' && uri[2] == 'e'
            && uri[3] == 'l' && uri[4] == 'l' && uri[5] == ':') {
        char buf[strlen(uri) + 1];
        webkit_policy_decision_ignore(WEBKIT_POLICY_DECISION(policy_decision));
        urldecode(buf, uri + 6);
        runshell(buf);
        return true;
    }
    return false;
}

// Re-run the background setter when the page loads.
void load_changed(WebKitWebView  *web_view,
        WebKitLoadEvent load_event, gpointer user_data) {
    if (load_event == WEBKIT_LOAD_FINISHED) {
        setbg();
        char buf[256];
        if (background_tint) {
            webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(webkit),
                "var $webwidget$ = document.createElement('div');"
                "$webwidget$.style.position = 'fixed';"
                "$webwidget$.style.top = 0;"
                "$webwidget$.style.left = 0;"
                "$webwidget$.style.right = 0;"
                "$webwidget$.style.bottom = 0;"
                "$webwidget$.style.zIndex = -1;"
                "$webwidget$.style.opacity = 0.25;"
                "document.body.appendChild($webwidget$);", NULL, NULL, NULL);
            sprintf(buf, "$webwidget$.style.background = '%s';",
                    background_tint);
            webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(webkit),
                buf, NULL, NULL, NULL);
        }
        if (adjust_text) {
            sprintf(buf, "document.body.style.color = '%s';",
                    (background_tint ? strcmp(background_tint, "white") : true) ? "white" : "black");
            webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(webkit),
                buf, NULL, NULL, NULL);
        }
    }
}

int help() {
    printf("Usage: %s [options...] URL\n", progname);
    puts("Display a web page like a desktop widget.");
    puts("");
    puts("  --desktop-background PATH  Show the right part of PATH in "
            "the background");
    puts("  --darken, --lighten        Tint the background showing through");
    puts("  --adjust-text              Lighten or darken text alongside "
            "above options");
    puts("  --geometry GEOM            Set the size and position from GEOM");
    puts("  --role TEXT                Set WM_WINDOW_ROLE to TEXT");
    puts("  --decorate                 Show window manager decorations");
    puts("  --allow-shell              Permit shell: protocol command "
            "execution");
    puts("  --stdin-js                 Read and execute code up to EOF on "
            "standard input");
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

gboolean read_stdin_line(GIOChannel *source, GIOCondition condition,
        gpointer data) {
    gchar *str_return;
    g_io_channel_read_to_end(source, &str_return, NULL, NULL);
    if (str_return) {
        webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(webkit), str_return, NULL, NULL, NULL);
    }
    return true;
}

int main (int argc, char **argv) {
    progname = argv[0];
    GtkAccelGroup *accelgroup;
    GClosure *closure;

    bool undecorated = true;
    char *role = NULL;
    char *geometry = "250x120";
    bool stdin_js;
    for (int i=1; i<argc; i++) {
        if (strcmp(argv[i], "--desktop-background") == 0)
            background_path = argv[++i];
        else if (strcmp(argv[i], "--geometry") == 0)
            geometry = argv[++i];
        else if (strcmp(argv[i], "--role") == 0)
            role = argv[++i];
        else if (strcmp(argv[i], "--allow-shell") == 0)
            allow_shell = true;
        else if (strcmp(argv[i], "--darken") == 0)
            background_tint = "black";
        else if (strcmp(argv[i], "--lighten") == 0)
            background_tint = "white";
        else if (strcmp(argv[i], "--adjust-text") == 0)
            adjust_text = true;
        else if (strcmp(argv[i], "--stdin-js") == 0)
            stdin_js = true;
        else if (strcmp(argv[i], "--decorate") == 0)
            undecorated = false;
        else if (strcmp(argv[i], "--version") == 0)
            exit(version());
        else if (strcmp(argv[i], "--help") == 0)
            exit(help());
        else if (argv[i][0] == '-') {
            fprintf(stderr, "webwidget: unknown option %s.\n", argv[i]);
            exit(1);
        } else
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
    // Decide whether to allow accessing URL, and handle shell: scheme.
    g_signal_connect(webkit, "decide-policy", G_CALLBACK(handle_policy), NULL);

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
    gtk_widget_show_all(webkit);

    // Set up widget window
    if (undecorated)
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    if (role)
        gtk_window_set_role(GTK_WINDOW(window), role);
 
    if (!gtk_window_parse_geometry(GTK_WINDOW(window), geometry)) {
        fprintf(stderr, "webwidget: could not parse geometry %s\n", geometry);
        exit(1);
    }
    int w, h;
    gtk_window_get_default_size(GTK_WINDOW(window), &w, &h);
    // If no size request is set, a non-resizable window becomes 1x1.
    if (w > 0 && h > 0)
        gtk_widget_set_size_request(window, w, h);
    else
        gtk_widget_set_size_request(window, 250, 120);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
 
    gtk_widget_show_all(window);
    // We check whether the desktop background has changed every 5 seconds
    if (background_path)
        g_timeout_add_seconds(5, timeout, NULL);

    // If --stdin-js was given, we will read JavaScript lines on stdin
    // and execute them.
    if (stdin_js) {
        GIOChannel *ioc = g_io_channel_unix_new(0);
        g_io_add_watch(ioc, G_IO_IN, &read_stdin_line, NULL);
    }

    gtk_main();
 
    return 0;
}
