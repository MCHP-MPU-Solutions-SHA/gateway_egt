/*
 * gateway.cpp
 *
 *  Created on: Oct 21, 2019
 *      Author: user
 */

#include <iostream>
#include <fstream>
#include <unistd.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sys/stat.h>
#include <functional>

#define EGT

#ifdef EGT
#include <egt/ui>
#endif
#include <mosquittopp.h>
extern "C" {
#include "cjson/cJSON.h"
}

using namespace std;
using namespace chrono;
#ifdef EGT
using namespace egt;
#endif

class Light
{
	public:
	enum {
		LIGHT_NONE = 0,
		LIGHT_OFF,
		LIGHT_ON,
		LIGHT_OFFLINE,
		LIGHT_ERROR
	} ;

	int status;
	string name;
	string mac;
	string topic_pub;
	string topic_sub;
	string temp;
	string hum;
	string uv;
	steady_clock::time_point tp;
	mutex mtx;

	Light() {
		status = LIGHT_NONE;
	}
	~Light() {
		status = LIGHT_NONE;
		name.erase();
		mac.erase();
		topic_pub.erase();
		topic_sub.erase();
		temp.erase();
		hum.erase();
		uv.erase();
	}
};

class Lights : public mosqpp::mosquittopp
{
	public:
		//const string config_file    = "/var/www/html/cgi-bin/lights.json";
		const string config_file     = "/var/www/cgi-bin/lights.json";
		const string json_lights     = "lights";
		const string json_name       = "name";
		const string json_mac        = "mac";
		const string json_topic_pub  = "topic_publish";
		const string json_topic_sub  = "topic_subscribe";
		const string mqtt_host       = "localhost";
		const    int mqtt_port       = 1883;
		const    int mqtt_alive      = 60;
		/* MQTT topic = root + /mac + sub/pub */
		const string mqtt_topic_root = "/end_node";
		const string mqtt_topic_sub  = "/sensor_board/data_report";
		const string mqtt_topic_pub  = "/sensor_board/data_control";
		const string mqtt_cmd_off    = "{\"id\":\"1\",\"version\":\"1.0\",\"params\":{\"light_switch\":0,\"light_intensity\":88,\"led_r\":0,\"led_g\":0,\"led_b\":0}}";
		const string mqtt_cmd_on     = "{\"id\":\"1\",\"version\":\"1.0\",\"params\":{\"light_switch\":1,\"light_intensity\":88,\"led_r\":0,\"led_g\":0,\"led_b\":0}}";

		int light_num;
		Light *lights;
		Light *lights_last;
		time_t config_timestamp;
		int mosq_connected;

		mutex mtx;
		condition_variable cond;

		Lights() {
			light_num = 0;
			lights = nullptr;
			lights_last = nullptr;
			config_timestamp = 0;
			mosq_connected = 0;
		}
		~Lights() {
			if (lights != nullptr)
				delete [] lights; 
		}

		int config_load();
		int config_guard();
		int light_update(const char *, Light *light);
		
		int display();

		int mosq_connect(void) {
			if (connect(mqtt_host.c_str(), mqtt_port, mqtt_alive))
				return -1;
			return loop_start();
		}

		int mosq_publish(const string& topic, const string& msg) {
			return publish(nullptr, topic.c_str(), msg.length(), msg.c_str(), 1, false);
		}

		int mosq_light_on(const string& mac) {
			auto topic = mqtt_topic_root + "/" + mac + mqtt_topic_pub;
			return mosq_publish(topic, mqtt_cmd_on);
		}

		int mosq_light_off(const string& mac) {
			auto topic = mqtt_topic_root + "/" + mac + mqtt_topic_pub;
			return mosq_publish(topic, mqtt_cmd_off);
		}
		
		void on_connect(int rc);
		void on_message(const struct mosquitto_message *msg);
		void on_disconnect(int rc);

	private:
};


void Lights::on_connect(int rc)
{
	if (!rc) {
		cout << "sub connected " << mqtt_topic_sub << endl;
		mosq_connected = 1;
		auto topic = mqtt_topic_root + "/+" + mqtt_topic_sub;
		subscribe(NULL, topic.c_str(), 0);
	}
}

void Lights::on_message(const struct mosquitto_message *msg)
{
	if(msg->payloadlen){
		//cout << (char *)msg->payload << endl;
		for (auto i=0; i<light_num; i++) {
			if (strstr(msg->topic, lights[i].mac.c_str())) {
				lock_guard<mutex> guard(mtx);
				light_update((char *)msg->payload, &lights[i]);
				cond.notify_one();
//cout << "update" << endl;
				//break;
			}
		}
	}
}

void Lights::on_disconnect(int rc)
{
	cout << "sub disconnect " << rc << endl;
	mosq_connected = 0;
}

int Lights::config_load()
{
	int result = -1;
	char *buffer;
	struct stat state;
	cJSON *json;
	cJSON *j_lights, *j_light, *j_name, *j_mac, *j_topic_pub, *j_topic_sub;
	ifstream infile;

	if (stat(config_file.c_str(), &state)) {
		cout << "ERROR: stat() " << config_file << endl;
		perror("stat()");
		return -1;
	}
	config_timestamp = state.st_mtime;

	buffer = new char[state.st_size+1];
	if (buffer == nullptr) {
		cout << "ERROR: new " << endl;
		return -1;
	}

	infile.open(config_file);
	if (!infile) {
		cout << "ERROR: open " << config_file << endl;
		goto EXIT;
	}

	infile.read(buffer, state.st_size);
	if (infile.gcount() != state.st_size) {
		cout << "ERROR: read " << infile.gcount() << ", total size " << state.st_size << endl;
		goto EXIT2;
	}

	//cout << buffer << endl;
	json = cJSON_Parse(buffer);
	if (!json) {
		cout << "ERROR: cJSON_Parse" << endl;
		goto EXIT2;
	}

	j_lights = cJSON_GetObjectItem(json, json_lights.c_str());
	if (!j_lights) {
		cout << "ERROR: cJSON_GetObjectItem lights" << endl;
		goto EXIT3;
	}

	if (j_lights->type != cJSON_Array) {
		cout << "ERROR: j_lights->type= " << j_lights->type << endl;
		goto EXIT3;
	}

	light_num = cJSON_GetArraySize(j_lights);
	if (light_num < 0) {
		cout << "Error light number: " << light_num << endl;
		goto EXIT3;
	} else if (light_num == 0) {
		result = 0;
		goto EXIT3;
	}

	lights = new Light[light_num*2];
	if (lights == nullptr) {
		cout << "ERROR: new Light[]" << endl;
		light_num = 0;
		goto EXIT3;
	}
	lights_last = lights + light_num;

	for (auto i=0; i<light_num; i++) {
		j_light = cJSON_GetArrayItem(j_lights, i);
		if (!j_light) {
			cout << "ERROR: cJSON_GetArrayItem " << i << endl;
			goto EXIT4;
		}

		j_name      = cJSON_GetObjectItem(j_light, json_name.c_str());
		j_mac       = cJSON_GetObjectItem(j_light, json_mac.c_str());
		j_topic_pub = cJSON_GetObjectItem(j_light, json_topic_pub.c_str());
		j_topic_sub = cJSON_GetObjectItem(j_light, json_topic_sub.c_str());
		if ((!j_name || !j_mac || !j_topic_pub || !j_topic_sub) || \
				(j_name->type != cJSON_String || \
					j_mac->type != cJSON_String || \
					j_topic_pub->type != cJSON_String || \
					j_topic_sub->type != cJSON_String)) {
			cout << "ERROR: get name and topic" << endl;
			goto EXIT4;
		}

		lights[i].name      = j_name->valuestring;
		lights[i].mac       = j_mac->valuestring;
		lights[i].topic_pub = j_topic_pub->valuestring;
		lights[i].topic_sub = j_topic_sub->valuestring;
		lights[i].tp        = steady_clock::now();
	}

	result = 0;
	goto EXIT3;
EXIT4:
	delete [] lights;
	lights = nullptr;
	lights_last = nullptr;
	light_num = 0;
EXIT3:
	cJSON_Delete(json);
EXIT2:
	infile.close();
EXIT:
	delete buffer;
	return result;
}

int Lights::config_guard()
{
	struct stat state;

	do {
		sleep(1);
		if (stat(config_file.c_str(), &state)) {
			cout << "ERROR: stat() " << config_file << endl;
			perror("stat()");
			continue;
		}

		if (state.st_mtime != config_timestamp)
			cout << "config file changed" << endl;
		config_timestamp = state.st_mtime;
	} while (1);

	return 0;
}

int Lights::light_update(const char *report, Light *light)
{
	int status; 
	string::size_type resize;
  cJSON *json_report, *json_params, *json_light, *json_temp, *json_hum, *json_uv;

//cout << "update() get " << report << endl;

	status = Light::LIGHT_ERROR;
	light->temp.resize(0);
	light->hum.resize(0);
	light->uv.resize(0);

	json_report = cJSON_Parse(report);
	if (json_report) {
		json_params = cJSON_GetObjectItem(json_report, "params");
		if (json_params) {
			json_light = cJSON_GetObjectItem(json_params, "light_switch");
			if (json_light) {
				if (json_light->valueint == 1)
					status = Light::LIGHT_ON;
				else if (json_light->valueint == 0)
					status = Light::LIGHT_OFF;
			}
			json_temp = cJSON_GetObjectItem(json_params, "temp");
			if (json_temp) {
				light->temp = to_string(json_temp->valuedouble);
				resize = light->temp.find(".");
				if (resize != string::npos)
					light->temp.resize(resize+3);
			}
			json_hum = cJSON_GetObjectItem(json_params, "hum");
			if (json_hum) {
				light->hum = to_string(json_hum->valueint);
			}
			json_uv = cJSON_GetObjectItem(json_params, "uv");
			if (json_uv) {
				light->uv = to_string(json_uv->valueint);
			}
		}
		cJSON_Delete(json_report);
	}

	light->status = status;
	light->tp     = steady_clock::now();

	return 0;
}

#ifdef EGT
int Lights::display()
{
	egt::Application app(0, nullptr);
	egt::TopWindow window;

	egt::ImageLabel img(Image("logo.png"));
	img.set_align(alignmask::left);
	window.add(img);

	egt::Label header("Home Automation Gateway",
								Rect(Point(0, 25), Size(window.width(), 100)),
								alignmask::left);
	header.set_font(Font(30, Font::weightid::bold));
	window.add(header);
	
	egt::ScrolledView view0(Rect(0, 100, window.width(), window.height()-100));
	view0.set_color(Palette::ColorId::bg, Palette::black);
	view0.set_name("view0");
	window.add(view0);

	egt::Label title("ID           Name            Light      Temperature    Humidity    Ultraviolet",
										Rect(Point(0, 0), Size(window.width(), 30)),
										alignmask::left);
	title.set_font(Font(20, Font::weightid::bold));
	view0.add(title);

	egt::TextBox     texts[light_num][6];
	egt::ToggleBox   buttons[light_num];
	for (auto i=0; i<light_num; i++) {
		// ID
		texts[i][0].set_box(Rect(Point(0, (i+1)*40), Size(30, 30)));
		texts[i][0].set_text(to_string(i+1).c_str());
		// Name
		texts[i][1].set_box(Rect(Point(52, (i+1)*40), Size(120, 30)));
		texts[i][1].set_text(lights[i].name.c_str());
		// Light
		texts[i][2].set_box(Rect(Point(169, (i+1)*40), Size(100, 30)));
		buttons[i].set_box(Rect(Point(184, (i+1)*40), Size(70, 30)));
		buttons[i].set_toggle_text("On", "Off");
		auto t      = this;
		auto last   = &lights_last[i];
		auto mac    = lights[i].mac;
		auto button = &buttons[i];
		auto handle = [t, last, mac, button](Event & event)
		{
			if (event.id() == eventid::property_changed) {
				//cout << "status " << button->checked() << endl;
				if (button->checked()) {
					if (last->status != Light::LIGHT_ON) {
						t->mosq_light_on(mac);
						cout << "Pub ON" << endl;
					}
				} else {
					if (last->status != Light::LIGHT_OFF) {
						t->mosq_light_off(mac);
						cout << "Pub OFF" << endl;
					}
				}
			}
		};
		buttons[i].on_event(handle);
		view0.add(buttons[i]); // Hidden with default
		//Temperature
		texts[i][3].set_box(Rect(Point(304, (i+1)*40), Size(60, 30)));
		texts[i][3].set_text(lights[i].temp.c_str());
		// Humidity
		texts[i][4].set_box(Rect(Point(422, (i+1)*40), Size(60, 30)));
		texts[i][4].set_text(lights[i].hum.c_str());
		// Ultraviolet
		texts[i][5].set_box(Rect(Point(530, (i+1)*40), Size(60, 30)));
		texts[i][5].set_text(lights[i].uv.c_str());

		for (auto j=0; j<6; j++) {
			texts[i][j].set_text_align(alignmask::center);
			texts[i][j].set_font(Font(20));
			texts[i][j].set_readonly(true);
			texts[i][j].set_border(false);
			view0.add(texts[i][j]);
		}
	}
	window.show();

	thread t(&egt::v1::Application::run, &app);
	t.detach();

	steady_clock::time_point t1, t2;
	do {
		t2 = steady_clock::now();
duration<double> time_span = duration_cast<duration<double>>(t2 - t1);
//cout << "It took me " << time_span.count() << " seconds." << endl;

		unique_lock<mutex> mlock(mtx);
		auto now = steady_clock::now();
		if (cond.wait_until(mlock, now + 1000ms) == cv_status::timeout) {
			auto tp = steady_clock::now();
			t1 = tp;
			for (auto i=0; i<light_num; i++) {
				if ((lights_last[i].status != Light::LIGHT_OFFLINE) &&
						(tp >= lights[i].tp + seconds(4))) {
					lights[i].status = Light::LIGHT_OFFLINE;
					lights[i].temp.resize(0);
					lights[i].hum.resize(0);
					lights[i].uv.resize(0);
				}
			}
//t2 = steady_clock::now();
		}

//cout << "display" << endl;
//t1 = steady_clock::now();
		for (auto i=0; i<light_num; i++) {
//t1 = steady_clock::now();
			if (lights[i].status != Light::LIGHT_NONE) {
				if (lights_last[i].status != lights[i].status) {
					if (lights[i].status == Light::LIGHT_OFFLINE) {
						if ((lights_last[i].status == Light::LIGHT_OFF) || \
								(lights_last[i].status == Light::LIGHT_ON))
							texts[i][2].zorder_top();	
							texts[i][2].set_text("Offline");
					} else if (lights[i].status == Light::LIGHT_ERROR) {
						if ((lights_last[i].status == Light::LIGHT_OFF) && \
								(lights_last[i].status == Light::LIGHT_ON))
							texts[i][2].zorder_top();
							texts[i][2].set_text("Error");
					} else {
						if ((lights_last[i].status == Light::LIGHT_OFFLINE) || \
							(lights_last[i].status == Light::LIGHT_ERROR) || \
								(lights_last[i].status == Light::LIGHT_NONE)) {
							texts[i][2].zorder_bottom();
							texts[i][2].clear();
						}
						// handle() shouldn't be triggered by set_checked() in class ToggleBox.
						lights_last[i].status = lights[i].status;
						if ((lights[i].status == Light::LIGHT_ON) && (!buttons[i].checked())) {
								buttons[i].set_checked(1);
								cout << "set checked 1" << endl;
						} else if ((lights[i].status == Light::LIGHT_OFF) && (buttons[i].checked())) {
								buttons[i].set_checked(0);
								cout << "set checked 0" << endl;
								//app.paint_to_file("./screen.jpg");
						}
					}
					lights_last[i].status = lights[i].status;
				}

				if (lights_last[i].temp != lights[i].temp) {
					texts[i][3].set_text(lights[i].temp.c_str());
					lights_last[i].temp = lights[i].temp;
				}

				if (lights_last[i].hum != lights[i].hum) {
//t1 = steady_clock::now();
					texts[i][4].set_text(lights[i].hum.c_str());
//t2 = steady_clock::now();
					lights_last[i].hum = lights[i].hum;
//t2 = steady_clock::now();
				}

				if (lights_last[i].uv != lights[i].uv) {
					texts[i][5].set_text(lights[i].uv.c_str());
					lights_last[i].uv = lights[i].uv;
				}

				lights[i].status = Light::LIGHT_NONE;	
			}
//t2 = steady_clock::now();
		}
//t2 = steady_clock::now();
	} while (1);

	return 0;
}
#endif

int main(int argc, const char** argv)
{
	Lights *gateway;

	gateway = new Lights;
	if (gateway->config_load()) {
		cout << "ERROR config_load" << endl;
		exit(EXIT_FAILURE);
	}

	thread t1(&Lights::config_guard, gateway);
	t1.detach();

	if (gateway->mosq_connect()) {
		cout << "ERROR mosq_connect()" << endl;
		exit(EXIT_FAILURE);
	}
#if 1
#ifdef EGT
	thread t(&Lights::display, gateway);
	//t.detach();
	t.join();
#endif
#endif
	while(1) {
		sleep(3);	
		gateway->mosq_light_on(gateway->lights[1].mac);
		sleep(3);
		gateway->mosq_light_off(gateway->lights[1].mac);
	}

	return 0;
}
