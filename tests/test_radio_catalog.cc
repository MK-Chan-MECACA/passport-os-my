#include "apps/radio/radio_catalog.h"
#include "apps/radio/radio_json_stream.h"
#include "apps/radio/radio_presets.h"
#include "esp_http_client.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <string>
#include <vector>

struct TestHttp { std::string url, body; std::size_t offset = 0; bool fail = false; };
std::vector<std::string> queries;
std::string city_fixture, province_fixture, country_fixture, ip_city, ip_region;
bool fail_dns = false, fail_first = false, fail_all = false, stopped = false, stop_on_read = false, fake_dns = false;
unsigned ip_requests = 0, live_http = 0;
uint32_t esp_random() { return 0; }
extern "C" esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }

std::string decode(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i+2 < s.size()) {
            char hex[3] = {s[i+1],s[i+2],0};out += char(std::strtoul(hex,nullptr,16));i+=2;
        } else out += s[i];
    }
    return out;
}
extern "C" esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    auto *client = new TestHttp;
    ++live_http;
    client->url = decode(config->url);
    client->fail = fail_all || (fail_first && client->url.find("de1.") != std::string::npos);
    if (client->url.find("ip-api") != std::string::npos || client->url.find("ipwho") != std::string::npos) {
        ++ip_requests;
        client->body = "{\"status\":\"success\",\"countryCode\":\"MY\",\"regionName\":\""+ip_region+"\",\"city\":\""+ip_city+"\"}";
    } else {
        queries.push_back(client->url);
        if (client->url.find("name=Johor&") != std::string::npos) client->body = province_fixture;
        else if (client->url.find("&name=") == std::string::npos) client->body = country_fixture;
        else client->body = city_fixture;
    }
    return client;
}
extern "C" esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_open(esp_http_client_handle_t c,int) { return c->fail ? ESP_FAIL : ESP_OK; }
extern "C" int esp_http_client_fetch_headers(esp_http_client_handle_t) { return 0; }
extern "C" int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
extern "C" int esp_http_client_read(esp_http_client_handle_t c,char *out,int capacity) {
    if (stop_on_read) stopped = true;
    auto n = std::min({std::size_t(capacity),std::size_t(7),c->body.size()-c->offset});
    std::memcpy(out,c->body.data()+c->offset,n);c->offset+=n;return int(n);
}
extern "C" bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->offset==c->body.size(); }
extern "C" esp_err_t esp_http_client_close(esp_http_client_handle_t) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { --live_http;delete c;return ESP_OK; }
extern "C" int getaddrinfo(const char *,const char *,const addrinfo *,addrinfo **out) {
    if(fail_dns)return EAI_FAIL;
    static sockaddr_in address;static addrinfo entry;
    address.sin_family=AF_INET;inet_pton(AF_INET,fake_dns?"198.18.1.101":"192.0.2.1",&address.sin_addr);
    entry={};entry.ai_family=AF_INET;entry.ai_addr=reinterpret_cast<sockaddr *>(&address);*out=&entry;
    return 0;
}
extern "C" void freeaddrinfo(addrinfo *) {}
bool cancel() { return stopped; }
std::string station(const std::string &name,const std::string &url,const std::string &extra="") {
    return "{\"name\":\""+name+"\",\"url\":\""+url+"\",\"codec\":\"MP3\""+extra+"}";
}
void reset() {
    assert(live_http==0);queries.clear();ip_requests=0;fail_dns=fail_first=fail_all=stopped=stop_on_read=fake_dns=false;
    ip_city="Johor Bahru";ip_region="Johor";city_fixture="[]";province_fixture="[]";country_fixture="[]";
}
void unique(const RadioStation *s,std::size_t n) {
    for(std::size_t i=0;i<n;++i)for(std::size_t j=i+1;j<n;++j)assert(std::strcmp(s[i].url,s[j].url));
}
int main(int argc,char **argv) {
    RadioStation stations[8] = {};RadioLocation preferred={},location={};
    std::strcpy(preferred.city,"Johor Bahru");std::strcpy(preferred.region,"Johor");
    reset();
    city_fixture="["+station("Johor FM 101.9","http://local/stream",",\"hls\":false,\"lastcheckok\":true")+","+
                      station("Best FM JB","http://local/2.mp3")+"]";
    province_fixture="[";
    for(int i=0;i<8;++i)province_fixture+=(i?",":"")+station("Johor state "+std::to_string(i),"http://province/"+std::to_string(i));
    province_fixture+="]";
    auto n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==8&&ip_requests==0&&queries.size()==2&&!std::strcmp(location.country_code,"MY"));
    assert(queries[0].find("countrycode=MY&name=Johor Bahru&")!=std::string::npos);
    assert(!std::strcmp(stations[0].name,"Johor FM 101.9")&&stations[0].frequency_decihz==1019);
    assert(std::strstr(stations[0].description,"Local")&&std::strstr(stations[2].description,"State"));unique(stations,n);

    // Voice handover stops after finding its target; browsing still tops up.
    // A missing named station must keep looking in the state directory.
    queries.clear();
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel,"");
    assert(n==2&&queries.size()==1&&!std::strcmp(stations[0].name,"Johor FM 101.9"));
    queries.clear();
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel,"Best");
    assert(n==2&&queries.size()==1);
    queries.clear();
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel,"Johor state");
    assert(n==8&&queries.size()==2);

    reset();
    city_fixture="["+station("一台","http://unique/live")+"]";
    province_fixture="["+station("重复","http://unique/live")+","+station("省台","http://province/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==2&&queries.size()==2);unique(stations,n);
    reset();country_fixture="[";
    for (int i=0;i<120;++i) {
        if (i) country_fixture+=",";
        country_fixture+=station("FM "+std::to_string(i),"http://country/"+std::to_string(i),",\"bitrate\":320");
    }
    country_fixture+=","+station("duplicate","http://country/119")+"]";
    RadioStationList list;
    assert(radio_catalog_discover_all(preferred,list,&location,cancel,"FM 119"));
    assert(list.size()==120+std::size(kRadioPresets));
    assert(queries.back().find("countrycode=MY&hidebroken=true")!=std::string::npos);
    assert(queries.back().find("bitrateMax=")==std::string::npos && queries.back().find("limit=")==std::string::npos);
    assert(!std::strcmp(list.at(list.size()-1).url,"http://country/119"));
    list.clear();
    reset();
    city_fixture="["+station("Johor local","http://local/live")+"]";
    country_fixture="["+station("Johor copy","http://local/live")+","+
        station("Preset copy",kRadioPresets[0].url)+","+station("National","http://national/live")+"]";
    assert(radio_catalog_discover_all(preferred,list,&location,cancel,"Johor"));
    assert(list.size()==std::size(kRadioPresets)+2 && queries.size()==2);
    assert(!std::strcmp(list.at(0).url,"http://local/live") &&
           !std::strcmp(list.at(list.size()-1).url,"http://national/live"));
    list.clear();
    reset();stop_on_read=true;
    assert(!radio_catalog_discover_all(preferred,list,&location,cancel) && !list.size() && !live_http);

    // IP providers use older or royal state spellings; both map to catalog states.
    reset();ip_city="Alor Gajah";ip_region="Malacca";city_fixture="["+station("Melaka FM","http://mk/live")+"]";
    n=radio_catalog_discover(stations,8,&location,cancel);
    assert(n==1&&ip_requests==1&&!std::strcmp(location.city,"Alor Gajah")&&!std::strcmp(location.region,"Melaka"));
    assert(queries[0].find("name=Alor Gajah&")!=std::string::npos&&queries[1].find("name=Melaka&")!=std::string::npos);
    reset();ip_city="Unknown Town";ip_region="Selangor Darul Ehsan";
    radio_catalog_discover(stations,8,&location,cancel);
    assert(!std::strcmp(location.region,"Selangor")&&!std::strcmp(location.country_code,"MY"));
    // A failed IP lookup falls back to Kuala Lumpur.
    reset();fail_all=true;radio_catalog_discover(stations,8,&location,cancel);
    assert(!std::strcmp(location.city,"Kuala Lumpur")&&!std::strcmp(location.region,"Kuala Lumpur"));
    assert(radio_catalog_known_place("kuala lumpur")&&radio_catalog_known_place("Penang")&&
           radio_catalog_known_place("Kota Kinabalu")&&!radio_catalog_known_place("Mars"));

    reset();fail_dns=true;fail_first=true;city_fixture="["+station("恢复","http://ok/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==1&&queries[0].find("de1.")!=std::string::npos&&queries[1].find("fi1.")!=std::string::npos);

    reset();fake_dns=true;city_fixture="["+station("代理网络","http://proxy/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==1&&queries[0].find("de1.")!=std::string::npos);

    reset();fail_all=true;
    assert(!radio_catalog_discover_all(preferred,list,&location,cancel));
    assert(list.size()==std::size(kRadioPresets));list.clear();
    reset();stop_on_read=true;city_fixture="["+station("取消","http://cancel/live")+"]";
    assert(radio_catalog_discover_location(preferred,stations,8,&location,cancel)==0&&live_http==0);
    auto old_queries=queries.size();assert(radio_catalog_discover_location(preferred,stations,8,&location,cancel)==0&&queries.size()==old_queries);

    reset();
    city_fixture="["+station(std::string(9000,'X'),"http://oversize/live")+","+
        station("坏台","http://bad/live",",\"lastcheckok\":false")+","+
        station("HLS台","http://hls/live",",\"hls\":true")+","+
        "{\"name\":\"AAC台\",\"url\":\"http://aac/live.mp3\",\"codec\":\"AAC\"},"+
        "{\"name\":\"Nasional FM 88,5 MHz\",\"url\":\"http://aacplus/live\",\"codec\":\"AAC+\"},"+
        "{\"name\":\"MP4\",\"url\":\"http://mp4/live.m4a\",\"codec\":\"AAC\"},"+
        "{\"name\":\"OGG\",\"url\":\"http://ogg/live\",\"codec\":\"OGG\"},"+
        station("长URL",std::string("http://url/")+std::string(300,'X'))+","+
        station("有效无后缀","http://valid/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==3&&!std::strcmp(stations[0].name,"AAC台")&&!std::strcmp(stations[1].name,"Nasional FM 88,5 MHz")&&
           stations[1].frequency_decihz==885&&!std::strcmp(stations[2].name,"有效无后缀")&&live_http==0);

    char object[32];RadioJsonStream parser(object,sizeof(object));std::vector<std::string> objects;
    std::string body="[{\"a\":\"}\\\"{\",\"b\":{}},{\"large\":\""+std::string(100,'x')+"\"},{\"last\":1}]";
    for(char c:body)parser.Feed(&c,1,[&](const char *json){objects.emplace_back(json);});
    assert(parser.Complete()&&objects.size()==2&&objects[1]=="{\"last\":1}");
    RadioJsonStream truncated(object,sizeof(object));truncated.Feed("[{",2,[](const char *){});assert(!truncated.Complete());
    RadioJsonStream invalid(object,sizeof(object));invalid.Feed("[{},]",5,[](const char *){});assert(!invalid.Complete());
    if(argc==2) {
        reset();
        std::ifstream country(argv[1]);assert(country);
        country_fixture.assign(std::istreambuf_iterator<char>(country),{});
        assert(radio_catalog_discover_all(preferred,list,&location,cancel));
        std::printf("Live Malaysian directory plus presets: %zu unique stream URLs\n",list.size());
        assert(list.size()>std::size(kRadioPresets));
    }
    std::puts("Radio: city priority, complete national list, preset fallback/dedup, mirror failover, cancellation, MP3/AAC metadata and bounded streamed JSON: PASS");
}
