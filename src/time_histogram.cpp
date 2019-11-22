/*
 Hanoh Haim
 Ido Barnea
 Cisco Systems, Inc.
*/

/*
Copyright (c) 2015-2016 Cisco Systems, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stdint.h>
#include <string.h>
#include "utl_json.h"
#include <rte_atomic.h>
#include "trex_global.h"
#include "time_histogram.h"
#include <iostream>
#include "hdr_histogram_log.h"


/* minimum value to track is 1 usec */
#define HDRH_LOWEST_TRACKABLE_VALUE 1
/* maximum value to track is 10 sec in usec unit */
#define HDRH_HIGHEST_TRACKABLE_VALUE (10*1000*1000)
/* precision in number of digits, 3 corresponds to 0.1% precision:
   1 usec precision up to 1000 usec
   1 msec precision or better up to 1 sec
   1 sec precision or better up to 1000 sec
*/
#define HDRH_SIGNIFICANT_DIGITS 3


void CTimeHistogram::Reset() {
    m_period_data[0].reset();
    m_period_data[1].reset();
    m_period = 0;
    m_total_cnt = 0;
    m_total_cnt_high = 0;
    m_max_dt = 0;
    m_min_dt = DBL_MAX;
    m_average = 0;
    memset(&m_max_ar[0],0,sizeof(m_max_ar));
    m_win_cnt = 0;

    int i;
    for (i = 0; i < HISTOGRAM_SIZE; i++) {
            m_hcnt[i] = 0;
    }
    if (m_hdrh) {
        hdr_reset(m_hdrh);
    }
}

bool CTimeHistogram::Create() {
	m_hdrh=0;
    if ( CGlobalInfo::m_options.m_hdrh ) {
        int res = hdr_init(HDRH_LOWEST_TRACKABLE_VALUE, HDRH_HIGHEST_TRACKABLE_VALUE,
                        HDRH_SIGNIFICANT_DIGITS, &m_hdrh);
        if (res) {
            return(false);
        }
    }
    Reset();
    m_min_delta =10.0/1000000.0;
    m_hot_max=10;
    return (true);
}

void CTimeHistogram::Delete() {
    if (m_hdrh) {
		hdr_close(m_hdrh);
        m_hdrh = 0;
    }
}

bool CTimeHistogram::Add(dsec_t dt) {
    CTimeHistogramPerPeriodData &period_elem = m_period_data[m_period];

    period_elem.inc_cnt();
    period_elem.update_sum(dt);
    if ((m_hot_max==0) || (m_total_cnt>m_hot_max) || (m_win_cnt > 1)){
        period_elem.update_max(dt);
        period_elem.update_min(dt);
    }

    // record any value in usec
    if (m_hdrh) {
        hdr_record_value(m_hdrh, (int64_t) (dt*1000000.0));
    }

    // values smaller then certain threshold do not get into the histogram
    if (dt < m_min_delta) {
        return false;
    }
    period_elem.inc_high_cnt();

		// Floating point latency in seconds to 10usec units 
    uint32_t d_10usec = (uint32_t)(dt*1000000.0) / HISTOGRAM_STEP;

		// What do we do otherwise?
		if(d_10usec<HISTOGRAM_SIZE)
        m_hcnt[d_10usec]++;
		
    return true;
}

void CTimeHistogram::update() {
    // switch period, and get values for pervious period
    CTimeHistogramPerPeriodData &period_elem = m_period_data[m_period];
    uint8_t new_period;

    // In case of two very fast reads, we do not want period with no
    // elements to influence the count. Also, when stream is stopped,
    // we want to preserve last values
    if (period_elem.get_cnt() == 0)
        return;

    if (m_period == 0) {
        new_period = 1;
    } else {
        new_period = 0;
    }
    m_period_data[new_period].reset();
    rte_mb();
    m_period = new_period;
    rte_mb();

    m_max_ar[m_win_cnt] = period_elem.get_max();
    m_win_cnt++;
    if (m_win_cnt == HISTOGRAM_QUEUE_SIZE) {
        m_win_cnt = 0;
    }
    update_average(period_elem);
    m_total_cnt += period_elem.get_cnt();
    m_total_cnt_high += period_elem.get_high_cnt();
    if ( m_max_dt < period_elem.get_max()) {
        m_max_dt = period_elem.get_max();
    }
    if ( m_min_dt > period_elem.get_min()) {
        m_min_dt = period_elem.get_min();
    }
}

void  CTimeHistogram::update_average(CTimeHistogramPerPeriodData &period_elem) {
    double c_average;

    if (period_elem.get_cnt() != 0) {
        c_average = period_elem.get_sum() / period_elem.get_cnt();
        // low pass filter
        m_average = 0.5 * m_average + 0.5 * c_average;
    }
}

dsec_t  CTimeHistogram::get_average_latency() {
    return (m_average);
}


uint32_t CTimeHistogram::get_usec(dsec_t d) {
    return (uint32_t)(d*1000000.0);
}

void CTimeHistogram::DumpWinMax(FILE *fd) {
    int i;
    uint32_t ci=m_win_cnt;

    for (i=0; i<HISTOGRAM_QUEUE_SIZE-1; i++) {
        dsec_t d=get_usec(m_max_ar[ci]);
        ci++;
        if (ci>HISTOGRAM_QUEUE_SIZE-1) {
            ci=0;
        }
        fprintf(fd," %.0f ",d);
    }
}

void CTimeHistogram::Dump(FILE *fd) {
    CTimeHistogramPerPeriodData &period_elem = m_period_data[get_read_period_index()];

    fprintf (fd," min_delta  : %lu usec \n", (ulong)get_usec(m_min_delta));
    fprintf (fd," cnt        : %lu \n", period_elem.get_cnt());
    fprintf (fd," high_cnt   : %lu \n", period_elem.get_high_cnt());
    fprintf (fd," max_d_time : %lu usec\n", (ulong)get_usec(m_max_dt));
    fprintf (fd," sliding_average    : %.0f usec\n", get_average_latency());
    fprintf (fd," precent    : %.1f %%\n",(100.0*(double)period_elem.get_high_cnt()/(double)period_elem.get_cnt()));

    fprintf (fd," histogram \n");
    fprintf (fd," -----------\n");
    int i;
    for (i = 0; i < HISTOGRAM_SIZE; i++) {
        if (m_hcnt[i] > 0) {
                fprintf (fd," h[%u]  :  %llu \n",(HISTOGRAM_STEP*(i+1)),(unsigned long long)m_hcnt[i]);
            }
    }
}

// Used in statefull
void CTimeHistogram::dump_json(std::string name,std::string & json ) {
    char buff[200];
    if (name != "")
        sprintf(buff,"\"%s\":{",name.c_str());
    else
        sprintf(buff,"{");
    json+=std::string(buff);

    json += add_json("min_usec", get_usec(m_min_delta));
    json += add_json("max_usec", get_usec(m_max_dt));
    json += add_json("high_cnt", m_total_cnt_high);
    json += add_json("cnt", m_total_cnt);
    json+=add_json("s_avg", get_average_latency());
    json+=add_json("s_max", get_max_latency_last_update());
    int i;

    json+=" \"histogram\": [ ";
    bool first=true;
		int start=0;
		int stop=0;
		int counter=0;
		int threshold= m_total_cnt * HISTOGRAM_THRESHOLD / 1000000;
		for (i = 0; i < HISTOGRAM_SIZE && counter < HISTOGRAM_THRESHOLD_CNT; i++) {
						
				if (first)
				{
						// We start to count
						if(m_hcnt[i])
						{
								start=i;
								first=false;
								json += std::to_string(m_hcnt[i]); 
						}
				}else{
						// Append the value as usual
						json += ", ";
						json += std::to_string(m_hcnt[i]);

						// Are we under the threshold?
						if(m_hcnt[i] < threshold)
						{
								// If it's the first element below our threshold
								if (!stop)
										stop = i;
								counter++;
						}else{
								if (stop)
								{
										// We were counting but there is a good element. Reset the structure.
										stop = 0;
										counter = 0;
								}
						}


				}

						/* json += add_json("key",(HISTOGRAM_STEP*(i+1))); */
						/* json += add_json("val",m_hcnt[i],true); */
						/* json += "}"; */
				}

		json+= " ], ";
		
		// usec of the first and last value reppresented by our histogram
		json += add_json("hist_start",start*10);
		json += add_json("hist_stop",i*10);
		
		// How much are above our stop? -> Can be considered a tail latency
		counter=0;
		for(; i<HISTOGRAM_SIZE; i++)
			counter+=m_hcnt[i];
		json += add_json("above",counter, true);


		json+=" },";
}

// Used in stateless
void CTimeHistogram::dump_json(Json::Value & json, bool add_histogram) {
    int i, rc;
    CTimeHistogramPerPeriodData &period_elem = m_period_data[get_read_period_index()];

    json["total_max"] = get_usec(m_max_dt);
    json["total_min"] = get_usec(m_min_dt);
    json["last_max"] = get_usec(period_elem.get_max());
    json["average"] = get_average_latency();

    if (add_histogram) {
				for (i = 0; i < HISTOGRAM_SIZE; i++) {
						if (m_hcnt[i] > 0) {
								std::string key = static_cast<std::ostringstream*>( &(std::ostringstream()
																																			<< int(HISTOGRAM_STEP * (i + 1)) ) )->str();
								json["histogram"][key] = Json::Value::UInt64(m_hcnt[i]);
						}
				}
        CTimeHistogramPerPeriodData &period_elem = m_period_data[m_period];
        if (m_total_cnt != m_total_cnt_high) {
            // since we are not running update on each get call now, we should also
            // take into account the values in current period
            uint64_t short_latency = m_total_cnt - m_total_cnt_high
                + period_elem.get_cnt() - period_elem.get_high_cnt();
            /* Since the incrementation between total_cnt and total_cnt_high is not atomic, short_latency isn't incremental.
            Therefore we force it to be incremental by taking the maximum. Maximum is calculated this way because there might
            be overflows. */
            int64_t difference = short_latency - m_short_latency;
            if (difference > 0) {
                m_short_latency = short_latency;
            }
            json["histogram"]["0"] = Json::Value::UInt64(m_short_latency);
        }

				//TODO: Check this part!
        if (m_hdrh) {
            char *hdr_encoded_histogram;

            // encode the hdr histogram in compressed base64 format
            rc = hdr_log_encode(m_hdrh, &hdr_encoded_histogram);
            if (rc == 0) {
                std::string hdr((const char *)hdr_encoded_histogram);
                free(hdr_encoded_histogram);
                json["hdrh"] = Json::Value(hdr);
            }
        }
    }
}

CTimeHistogram CTimeHistogram::operator+= (const CTimeHistogram& in) {
    for (uint16_t i = 0; i < HISTOGRAM_SIZE; i++) {
            this->m_hcnt[i] += in.m_hcnt[i];
        }
    for (uint i = 0; i < HISTOGRAM_QUEUE_SIZE; i++) {
        this->m_max_ar[i] = std::max(this->m_max_ar[i], in.m_max_ar[i]);
    }
    for (uint8_t i = 0 ; i < 2; i++) {
        this->m_period_data[i] += in.m_period_data[i];
    }
    this->m_total_cnt += in.m_total_cnt;
    this->m_total_cnt_high += in.m_total_cnt_high;
    uint64_t new_sum = 0;
    for (uint8_t i = 0; i < 2; i++) {
        new_sum += this->m_period_data[i].get_sum();
    }
    this->m_max_dt = std::max(this->m_max_dt, in.m_max_dt);
    this->m_min_dt = std::min(this->m_min_dt, in.m_min_dt);
    this->m_win_cnt = in.m_win_cnt;
    this->m_period = in.m_period;
    if (this->m_total_cnt != 0) {
        // low pass filter
        this->m_average = 0.5 * this->m_average + 0.5 * (new_sum / this->m_total_cnt);
    }
    return *this;
}

std::ostream& operator<<(std::ostream& os, const CTimeHistogram& in) {
    os << "m_total count << " << in.m_total_cnt << std::endl;
    os << "m_total_count_high" << in.m_total_cnt_high << std::endl;
    os << "m_average" << in.m_average << std::endl;
    // Other things might be added.
    return os;
}

