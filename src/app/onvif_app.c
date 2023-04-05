#include "onvif_app.h"
#include "credentials_input.h"
#include "onvif_details.h"
#include "onvif_nvt.h"
#include "device_list.h"
#include "../queue/event_queue.h"
#include "../gst/player.h"
#include "discoverer.h"

typedef struct _OnvifApp {
    Device* device; /* Currently selected device */
    DeviceList *device_list;

    GtkWidget *listbox;
    GtkWidget *main_notebook;
    GtkWidget *player_loading_handle;

    CredentialsDialog * dialog;

    int current_page;

    OnvifDetails * details;
    EventQueue * queue;
    RtspPlayer * player;
} OnvifApp;

struct PlayInput {
    OnvifApp * app;
    Device * device;
    GtkListBoxRow * row;
};

struct DeviceInput {
    Device * device;
    OnvifApp * app;
};

struct DiscoveryInput {
    OnvifApp * app;
    GtkWidget * widget;
};

/*
 *
 *  Gtk GUI Signal Callbacks
 *
 */

/* This function is called when the STOP button is clicked */
static void quit_cb (GtkButton *button, RtspPlayer *data) {
    RtspPlayer__stop(data);
    gtk_main_quit();
}

/* This function is called when the main window is closed */
static void delete_event_cb (GtkWidget *widget, GdkEvent *event, RtspPlayer *data) {
    RtspPlayer__stop(data);
    gtk_main_quit ();
}

static gboolean * finished_discovery (void * e) {
    DiscoveryEvent * event = (DiscoveryEvent *) e;
    struct DiscoveryInput * disco_in = (struct DiscoveryInput * ) event->data;
    
    gtk_widget_set_sensitive(disco_in->widget,TRUE);
    free(disco_in);
    return FALSE;
}

void add_device(OnvifApp * self, char * uri, char* name, char * hardware, char * location);

static gboolean * found_server (void * e) {
    DiscoveryEvent * event = (DiscoveryEvent *) e;

    DiscoveredServer * server = event->server;
    struct DiscoveryInput * disco_in = (struct DiscoveryInput * ) event->data;

    ProbMatch * m;
    int i;
  
    printf("Found server ---------------- %i\n",server->matches->match_count);
    for (i = 0 ; i < server->matches->match_count ; ++i) {
        m = server->matches->matches[i];
        add_device(disco_in->app, g_strdup(m->addrs[0]), onvif_extract_scope("name",m), onvif_extract_scope("hardware",m), onvif_extract_scope("location",m));
    }

    return FALSE;
}

void onvif_scan (GtkWidget *widget, OnvifApp * app) {

    gtk_widget_set_sensitive(widget,FALSE);

    //Clearing the list
    gtk_container_foreach (GTK_CONTAINER (app->listbox), (GtkCallback)gtk_widget_destroy, NULL);
    DeviceList__clear(app->device_list);

    struct DiscoveryInput * disco_in = malloc(sizeof(struct DiscoveryInput));
    disco_in->app = app;
    disco_in->widget = widget;
    //Start UDP Scan
    struct UdpDiscoverer discoverer = UdpDiscoverer__create(found_server,finished_discovery);
    UdpDiscoverer__start(&discoverer, disco_in);

}

void error_onvif_stream(RtspPlayer * player, void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    gtk_spinner_stop (GTK_SPINNER (app->player_loading_handle));
}

void stopped_onvif_stream(RtspPlayer * player, void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    gtk_spinner_stop (GTK_SPINNER (app->player_loading_handle));
}

void start_onvif_stream(RtspPlayer * player, void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    gtk_spinner_start (GTK_SPINNER (app->player_loading_handle));
}

void _retry_onvif_stream(void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    sleep(1);
    RtspPlayer__retry(app->player);
}

void retry_onvif_stream(RtspPlayer * player, void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    EventQueue__insert(app->queue,_retry_onvif_stream,app);
}

void _stop_onvif_stream(void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    RtspPlayer__stop(app->player);
}

void _display_onvif_device(void * user_data){
    struct DeviceInput * input = (struct DeviceInput *) user_data;
    /* Start by authenticating the device then start retrieve thumbnail */
    OnvifDevice_authenticate(input->device->onvif_device);

    /* Display Profile dropdown */
    // Device__load_profiles(input->device,input->app->queue);

    /* Display row thumbnail. Default to profile index 0 */
    Device__load_thumbnail(input->device,input->app->queue);

    free(input);
}

gboolean * gdk_thread_dispatch (void * user_data){
    struct PlayInput * input = (struct PlayInput *) user_data;
    CredentialsDialog__show(input->app->dialog,input);
    return FALSE;
}

void onvif_display_device_row(OnvifApp * self, Device * device){
    struct DeviceInput * input = malloc(sizeof(struct DeviceInput));
    input->device = device;
    input->app = self;

    /* nslookup doesn't require onvif authentication. Dispatch event now. */
    Device__lookup_hostname(self->device,self->queue);

    EventQueue__insert(self->queue,_display_onvif_device,input);
}

void _play_onvif_stream(void * user_data){
    struct PlayInput * input = (struct PlayInput *) user_data;

    //Check if device is still valid. (User performed scan before thread started)
    if(!Device__addref(input->device)){
        goto exit;
    }

    if(!input->device->selected){
        return;
    }

        /* Set the URI to play */
        //TODO handle profiles
    char * uri = OnvifDevice__media_getStreamUri(input->device->onvif_device,0);

    RtspPlayer__set_playback_url(input->app->player,uri);

    //User performed scan before StreamURI was retrieved
    if(!Device__is_valid(input->device) || !input->device->selected){
        goto exit;
    }

    RtspPlayer__play(input->app->player);

exit:
    Device__unref(input->device);
    free(input);
}

void update_details(OnvifApp * self){
    if(!self->current_page){//NVT is displayed
        return;
    }
    OnvifDetails_update_details(self->details,self->device);
}

void OnvifApp__select_device(OnvifApp * app,  GtkListBoxRow * row){
    struct PlayInput * input = malloc (sizeof(struct PlayInput));
    input->app = app;
    input->row = row;

    OnvifDetails_clear_details(input->app->details);
    EventQueue__insert(app->queue,_stop_onvif_stream,app);
    if(input->app->device){
        input->app->device->selected = 0;
    }

    if(input->row == NULL){
        input->app->device = NULL;
        free(input);
        return;
    }

    //Set newly selected device
    int pos;
    pos = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (input->row));
    input->app->device = input->app->device_list->devices[pos];
    input->app->device->selected = 1;
    input->device = input->app->device;

    //Prompt for authentication
    if(!input->device->onvif_device->authorized){
        gdk_threads_add_idle((void *)gdk_thread_dispatch,input);
        return;
    }

    EventQueue__insert(app->queue,_play_onvif_stream,input);
    update_details(input->app);
}

void row_selected_cb (GtkWidget *widget,   GtkListBoxRow* row,
  OnvifApp* app)
{
    OnvifApp__select_device(app,row);    
}

static void switch_page (GtkNotebook* self, GtkWidget* page, guint page_num, OnvifApp * app) {
    OnvifDetails_clear_details(app->details);
    app->current_page = page_num;
    update_details(app);
}

/*
 *
 *  UI Creation
 *
 */
void create_ui (OnvifApp * app) {
    GtkWidget *main_window;  /* The uppermost window, containing all other windows */
    GtkWidget *grid;
    GtkWidget *widget;
    GtkWidget *label;
    GtkWidget * hbox;

    main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), app->player);

    gtk_window_set_title (GTK_WINDOW (main_window), "Onvif Device Manager");

    GtkWidget * overlay =gtk_overlay_new();
    gtk_container_add (GTK_CONTAINER (main_window), overlay);

    /* Here we construct the container that is going pack our buttons */
    grid = gtk_grid_new ();
    gtk_container_set_border_width (GTK_CONTAINER (grid), 10);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),grid);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),app->dialog->root);

    widget = gtk_button_new ();
    label = gtk_label_new("");
    gtk_label_set_markup (GTK_LABEL (label), "<span size=\"x-large\">Scan</span>...");
    gtk_container_add (GTK_CONTAINER (widget), label);
    gtk_widget_set_vexpand (widget, FALSE);
    gtk_widget_set_hexpand (widget, FALSE);
    gtk_widget_set_size_request(widget,-1,80);
    gtk_grid_attach (GTK_GRID (grid), widget, 0, 0, 1, 1);
    g_signal_connect (widget, "clicked", G_CALLBACK (onvif_scan), app);

    /* --- Create a list item from the data element --- */
    widget = gtk_scrolled_window_new (NULL, NULL);
    app->listbox = gtk_list_box_new ();
    gtk_widget_set_size_request(widget,200,-1);
    gtk_widget_set_size_request(app->listbox,200,-1);
    gtk_widget_set_vexpand (app->listbox, TRUE);
    gtk_widget_set_hexpand (app->listbox, FALSE);
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (app->listbox), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(widget),app->listbox);
    gtk_grid_attach (GTK_GRID (grid), widget, 0, 1, 1, 1);
    g_signal_connect (app->listbox, "row-selected", G_CALLBACK (row_selected_cb), app);

    widget = gtk_button_new_with_label ("Quit");
    gtk_widget_set_vexpand (widget, FALSE);
    gtk_widget_set_hexpand (widget, FALSE);
    g_signal_connect (G_OBJECT(widget), "clicked", G_CALLBACK (quit_cb), app);
    gtk_grid_attach (GTK_GRID (grid), widget, 0, 2, 1, 1);

    //Padding (TODO Implement draggable resizer)
    widget = gtk_label_new("");
    gtk_widget_set_vexpand (widget, TRUE);
    gtk_widget_set_hexpand (widget, FALSE);
    gtk_widget_set_size_request(widget,20,-1);
    gtk_grid_attach (GTK_GRID (grid), widget, 1, 0, 1, 3);

    /* Create a new notebook, place the position of the tabs */
    app->main_notebook = gtk_notebook_new ();
    gtk_notebook_set_tab_pos (GTK_NOTEBOOK (app->main_notebook), GTK_POS_TOP);
    gtk_widget_set_vexpand (app->main_notebook, TRUE);
    gtk_widget_set_hexpand (app->main_notebook, TRUE);
    gtk_grid_attach (GTK_GRID (grid), app->main_notebook, 2, 0, 1, 3);

    char * TITLE_STR = "NVT";
    label = gtk_label_new (TITLE_STR);

    //Hidden spinner used to display stream start loading
    app->player_loading_handle = gtk_spinner_new ();

    //Only show label and keep loading hidden
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start (GTK_BOX(hbox),label,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(hbox),app->player_loading_handle,FALSE,FALSE,0);
    gtk_widget_show_all(hbox);

    widget = OnvifNVT__create_ui(app->player);
    gtk_notebook_append_page (GTK_NOTEBOOK (app->main_notebook), widget, hbox);

    label = gtk_label_new ("Details");

    //Hidden spinner used to display stream start loading
    widget = gtk_spinner_new ();
    OnvifDetails_set_details_loading_handle(app->details,widget);

    //Only show label and keep loading hidden
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start (GTK_BOX(hbox),label,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(hbox),widget,FALSE,FALSE,0);
    gtk_widget_show_all(hbox);

    widget = OnvifDetails__get_widget(app->details);

    gtk_notebook_append_page (GTK_NOTEBOOK (app->main_notebook), widget, hbox);

    GdkRectangle workarea = {0};
    gdk_monitor_get_workarea(
        gdk_display_get_primary_monitor(gdk_display_get_default()),
        &workarea);

    if(workarea.width > 775){//If resolution allows it, big enough to show no scrollbars
        gtk_window_set_default_size(GTK_WINDOW(main_window),775,480);
    } else {//Resolution is so low that we launch fullscreen
        gtk_window_set_default_size(GTK_WINDOW(main_window),workarea.width,workarea.height);
    }

    gtk_widget_show_all (main_window);
    g_signal_connect (G_OBJECT (app->main_notebook), "switch-page", G_CALLBACK (switch_page), app);

}

void _onvif_authentication(void * user_data){
    LoginEvent * event = (LoginEvent *) user_data;
    struct PlayInput * input = (struct PlayInput *) event->user_data;
    //Check device is still valid before adding ref (User performed scan before thread started)
    if(!Device__addref(input->device)){
        goto exit;
    }

    OnvifDevice_set_credentials(input->device->onvif_device,event->user,event->pass);
    OnvifDevice_authenticate(input->device->onvif_device);

    //Check if device is valid and authorized (User performed scan before auth finished)
    if(!Device__is_valid(input->device) || !input->device->onvif_device->authorized || !input->device->selected){
        goto exit;
    }

    //Replace locked image with spinner
    GtkWidget * image = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (image));
    gtk_container_foreach (GTK_CONTAINER (input->device->image_handle), (void*) gtk_widget_destroy, NULL);
    gtk_container_add (GTK_CONTAINER (input->device->image_handle), image);
    gtk_widget_show (image);

    OnvifDetails_clear_details(input->app->details);
    CredentialsDialog__hide(input->app->dialog);
    onvif_display_device_row(input->app, input->device);
    EventQueue__insert(input->app->queue,_play_onvif_stream,input); //Input is cleaned up here
    update_details(input->app);

exit:
    Device__unref(input->device);
    free(event);
}

void dialog_cancel_cb(CredentialsDialog * dialog){
    printf("OnvifAuthentication cancelled...\n");
    CredentialsDialog__hide(dialog);
    free(dialog->user_data);
}

void dialog_login_cb(LoginEvent * event){
    printf("OnvifAuthentication attempt...\n");
    struct PlayInput * input = (struct PlayInput *) event->user_data;
    EventQueue__insert(input->app->queue,_onvif_authentication,LoginEvent_copy(event));
}

OnvifApp * OnvifApp__create(){
    OnvifApp *app  =  malloc(sizeof(OnvifApp));
    app->player = RtspPlayer__create();
    app->device_list = DeviceList__create();
    app->device = NULL;
    app->dialog = CredentialsDialog__create(dialog_login_cb, dialog_cancel_cb);
    app->queue = EventQueue__create();
    app->details = OnvifDetails__create(app->queue);
    app->current_page = 0;

    //Defaults 4 paralell event threads.
    //TODO support configuration to modify this
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);
    EventQueue__start(app->queue);

    RtspPlayer__set_retry_callback(app->player, retry_onvif_stream, app);
    RtspPlayer__set_error_callback(app->player, error_onvif_stream, app);
    RtspPlayer__set_stopped_callback(app->player, stopped_onvif_stream, app);
    RtspPlayer__set_start_callback(app->player, start_onvif_stream, app);
    
    create_ui (app);

    return app;
}

void OnvifApp__destroy(OnvifApp* self){
    if (self) {
        OnvifDetails__destroy(self->details);
        CredentialsDialog__destroy(self->dialog);
        RtspPlayer__destroy(self->player);
        EventQueue__destroy(self->queue);
        DeviceList__destroy(self->device_list);
        free(self);
    }
}

void add_device(OnvifApp * self, char * uri, char* name, char * hardware, char * location){
    OnvifDevice * onvif_dev = OnvifDevice__create(uri);
    Device * device = Device__create(onvif_dev);
    DeviceList__insert_element(self->device_list,device,self->device_list->device_count);
    int b;
    for (b=0;b<self->device_list->device_count;b++){
        printf("DEBUG List Record :[%i] %s:%s\n",b,self->device_list->devices[b]->onvif_device->ip,self->device_list->devices[b]->onvif_device->port);
    }

    GtkWidget * row = Device__create_row(device, uri, name, hardware, location);
    
    gtk_list_box_insert (GTK_LIST_BOX (self->listbox), row, -1);
    gtk_widget_show_all (row);

    onvif_display_device_row(self,device);
}