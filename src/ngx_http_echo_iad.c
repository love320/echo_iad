#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_http_echo_iad.h"
#include "ngx_http_echo_util.h"
#include "ngx_http_echo_filter.h"
#include "ngx_shmap.h"


#include <nginx.h>

static ngx_buf_t ngx_http_echo_space_buf;

static ngx_buf_t ngx_http_echo_newline_buf;

static ngx_shm_zone_t* zone ;//缓存集

static ngx_str_t data_map_key_defalue = ngx_null_string;//默认key 999999

static ngx_str_t ngx_http_iad_domain = ngx_null_string;//网关主-备 信息

static ngx_str_t* ngx_http_echo_iad_str_palloc(ngx_http_request_t *r);//专用 ngx_str_t 申请内存器并初始化

static ngx_str_t* ngx_http_echo_iad_timestamp(ngx_http_request_t *r); //处理时间格式，专用 gateway

static ngx_int_t ngx_http_echo_iad_arg(ngx_http_request_t *r, u_char *name, size_t len,ngx_str_t *value);//通用获取参数信息 支持get post

static ngx_str_t* ngx_http_echo_iad_cache_key(ngx_http_request_t *r,ngx_str_t *gateway,ngx_str_t *type);//合成缓存key值信息

void ngx_http_echo_iad_foreach_pt(ngx_shmap_node_t* node, void* extarg);//遍例所有缓存数据方法，使用非static方法，使其被外部使用。



ngx_int_t
ngx_http_echo_iad_init(ngx_conf_t *cf)
{
    static u_char space_str[]   = " ";
    static u_char newline_str[] = "\n";

    dd("global init...");

    ngx_memzero(&ngx_http_echo_space_buf, sizeof(ngx_buf_t));

    ngx_http_echo_space_buf.memory = 1;

    ngx_http_echo_space_buf.start =
        ngx_http_echo_space_buf.pos =
            space_str;

    ngx_http_echo_space_buf.end =
        ngx_http_echo_space_buf.last =
            space_str + sizeof(space_str) - 1;

    ngx_memzero(&ngx_http_echo_newline_buf, sizeof(ngx_buf_t));

    ngx_http_echo_newline_buf.memory = 1;

    ngx_http_echo_newline_buf.start =
        ngx_http_echo_newline_buf.pos =
            newline_str;

    ngx_http_echo_newline_buf.end =
        ngx_http_echo_newline_buf.last =
            newline_str + sizeof(newline_str) - 1;


    size_t shm_size =1024*1024*10; //设置缓存大小 1024*1024*10 = 10M
    ngx_str_t iad_shm_name = ngx_string("shm_iad_cache_zone"); //设置缓存的名字
    zone = ngx_shmap_init(cf,&iad_shm_name,shm_size,&ngx_http_echo_module);//初始化缓存对象

    ngx_str_set(&data_map_key_defalue,"999999");//默认key值
    ngx_str_set(&ngx_http_iad_domain,"domain");//默认key值

    return NGX_OK;
}


ngx_int_t
ngx_http_echo_exec_iad_sync(ngx_http_request_t *r,
    ngx_http_echo_ctx_t *ctx)
{
    ngx_buf_t                   *buf;
    ngx_chain_t                 *cl = NULL; /* the head of the chain link */

    buf = ngx_calloc_buf(r->pool);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    buf->sync = 1;

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cl->buf  = buf;
    cl->next = NULL;

    return ngx_http_echo_send_chain_link(r, ctx, cl);
}


ngx_int_t
ngx_http_echo_exec_iad(ngx_http_request_t *r,
    ngx_http_echo_ctx_t *ctx, ngx_array_t *computed_args,
    ngx_flag_t in_filter, ngx_array_t *opts)
{
    ngx_uint_t                  i;

    ngx_buf_t                   *space_buf;
    ngx_buf_t                   *newline_buf;
    ngx_buf_t                   *buf;

    ngx_str_t                   *computed_arg;
    ngx_str_t                   *computed_arg_elts;
    ngx_str_t                   *opt;

    ngx_chain_t *cl  = NULL; /* 链的链接 */
    ngx_chain_t **ll = &cl;  /* 总是指向最后一个链接的地址 */

    ngx_str_t                   *data_map_key;
    ngx_str_t                   *data_map_value;
    //ngx_str_t                   data_map_value_null = ngx_null_string;//此信息不加入r->pool中，这个是缓存值不释放
    
    ngx_str_t                   *s_domain;
    ngx_str_t                   *s_gateway;
    ngx_str_t                   *s_app_type;
    ngx_str_t                   *s_szn;
    ngx_str_t                   *s_data;
    ngx_str_t                   *s_iad;
    ngx_int_t                   i_action = 0; 

    size_t                      str_json_len;
    u_char                      *str_json_data_p;
    ngx_str_t                   *ngx_str_send_json;

    ngx_int_t                   i_state = 0;

   
    ngx_str_t                   *all_key_json_data;//存放遍例后的所有key信息为一个字符串(大字符串)
    ngx_http_iad_all_key_json_data_t *key_json_r; //结构体存放，ngx_http_request_t指定和持追加字符信息(all_key_json_data)。

    //暂时 --删除代码
    //ngx_http_iad_timestamp(r);


    s_domain = ngx_http_echo_iad_str_palloc(r);
    s_gateway = ngx_http_echo_iad_str_palloc(r);
    s_app_type = ngx_http_echo_iad_str_palloc(r);
    s_szn = ngx_http_echo_iad_str_palloc(r);
    s_data = ngx_http_echo_iad_str_palloc(r);
    s_iad = ngx_http_echo_iad_str_palloc(r);
    all_key_json_data = ngx_http_echo_iad_str_palloc(r);

    uint8_t vt_str_cache = VT_STRING;    
    
    if (computed_args == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /*
    if(!(r->method & (NGX_HTTP_GET|NGX_HTTP_POST))){
        return NGX_HTTP_NOT_FOUND;
    } */ 

    /* 获取 mid 参数信息 */
    if(ngx_http_echo_iad_arg(r, (u_char*)"gateway", 7, s_gateway)!=NGX_OK){};

    /* appType */
    if(ngx_http_echo_iad_arg(r, (u_char*)"appType", 7, s_app_type)!=NGX_OK){i_state = -201;}//应用APP信息 没有

    /* 获取 action 参数信息 */
    if(ngx_http_echo_iad_arg(r, (u_char*)"action", 6, s_szn)==NGX_OK){
        i_action = ngx_atoi(s_szn->data, s_szn->len);    
    }

    //若用户是注册，则action = 1;
    if(i_action == 0 && s_gateway->len == 0) i_action = 1;

    /* 获取key信息 */
    data_map_key = ngx_http_echo_iad_cache_key(r,s_gateway,s_app_type);

    //data_map_key = s_gateway;
   // data_map_value = &data_map_value_null;
    data_map_value = ngx_http_echo_iad_str_palloc(r);

    
    ngx_shmap_get(zone,&ngx_http_iad_domain, s_domain,&vt_str_cache,0,0);//检测key_domain是否存在
    //若是设置网关主备信息，则通过.可以跳过上判断
    if(i_action == 501){
        i_state = 0;
    }else{
        if(s_domain->len <= 0){i_state = 501;i_action = -1;}//网关域名未初始化
    }

    if(i_state != 0){ i_action = -1;}//状态不满足需求，不处理。i_action = -1 跳过处理
    

    computed_arg_elts = computed_args->elts;



    for (i = 0; i < computed_args->nelts; i++) {
        computed_arg = &computed_arg_elts[i];

        switch(i_action){
            case 0:{
                if(data_map_key->len <= 0){i_state = -1;break;}//无key信息
                /*  从zone 缓存中查找对象 */
                ngx_shmap_get(zone,data_map_key, data_map_value,&vt_str_cache,0,0);

                //没有找到
                if(data_map_value->len <= 0) { //缓存信息中没有找到
                //i_state = 2;
                    data_map_key = ngx_http_echo_iad_cache_key(r,&data_map_key_defalue,s_app_type);//默认key

                    //使用默认 999999
                    ngx_shmap_get(zone,data_map_key, data_map_value,&vt_str_cache,0,0);
                } 

                break;
            }
                    
            case 1:{
                s_gateway = ngx_http_echo_iad_timestamp(r);//分配注册信息

                data_map_key = ngx_http_echo_iad_cache_key(r,s_gateway,s_app_type);//获取分配注册key信息

                ngx_shmap_get(zone,data_map_key, data_map_value,&vt_str_cache,0,0);//从缓存查找信息

                //没有找到
                if(data_map_value->len <= 0) { //缓存信息中没有找到 
                    data_map_key = ngx_http_echo_iad_cache_key(r,&data_map_key_defalue,s_app_type);//默认key

                    //使用默认 999999
                    ngx_shmap_get(zone,data_map_key, data_map_value,&vt_str_cache,0,0);
                    if(data_map_value->len <= 0) i_state = 4;//默认信息未初始化
                } 

                break;
            }

            case 101:{      
                if(!(r->method & NGX_HTTP_POST)){i_state = -120;break;}//新增操作必需为post方式          
                if(ngx_http_echo_iad_arg(r, (u_char*)"iad", 3, s_iad)!=NGX_OK){i_state = -101;break;}//无参数iad - 密钥                 
                if(s_iad->len != computed_arg->len){ i_state = -102;break;}//参数iad - 密钥 长度不一致 
                if(ngx_strncmp(s_iad->data,computed_arg->data,computed_arg->len) != 0){ i_state = -103;break;}//参数iad 不等于 密钥
                if(ngx_http_echo_iad_arg(r, (u_char*)"data", 4, s_data)!=NGX_OK){ i_state = -104;break;}//无参数data信息

                ngx_shmap_get(zone,data_map_key, data_map_value,&vt_str_cache,0,0);//检测key是否存在
                if(data_map_value->len > 0){ i_state = -105;break;}//key 对应 value 已存在

                ngx_str_set(data_map_value,s_data->data);//装载data信息                 
                data_map_value->len = s_data->len;//设置信息长度
                ngx_shmap_add(zone, data_map_key,data_map_value,VT_STRING,0,0);//加入缓存数据

                break;
            }

            case 102:{             
                if(ngx_http_echo_iad_arg(r, (u_char*)"iad", 3, s_iad)!=NGX_OK){i_state = -101;break;}//无参数iad - 密钥 
                if(s_iad->len != computed_arg->len){ i_state = -102;break;}//参数iad - 密钥 长度不一致 
                if(ngx_strncmp(s_iad->data,computed_arg->data,computed_arg->len) != 0){ i_state = -103;break;}//参数iad 不等于 密钥

                ngx_shmap_get(zone,data_map_key, data_map_value,&vt_str_cache,0,0);//检测key是否存在
                if(data_map_value->len <= 0){ i_state = -106;break;}//移除的 key 对应 value 不存在

                ngx_shmap_delete(zone,data_map_key);//移除缓存数据      
                break;
            }

            
            case 111:{

                if(ngx_http_echo_iad_arg(r, (u_char*)"iad", 3, s_iad)!=NGX_OK){i_state = -101;break;}//无参数iad - 密钥 
                if(s_iad->len != computed_arg->len){ i_state = -102;break;}//参数iad - 密钥 长度不一致 
                if(ngx_strncmp(s_iad->data,computed_arg->data,computed_arg->len) != 0){ i_state = -103;break;}//参数iad 不等于 密钥

                key_json_r = ngx_palloc(r->pool,sizeof(ngx_http_iad_all_key_json_data_t));//分配内存
                key_json_r->all_key = all_key_json_data;//申明信息装载变量
                key_json_r->r = r;//申明请求信息变量
                ngx_shmap_foreach(zone,ngx_http_echo_iad_foreach_pt,key_json_r);//进行遍例缓存所有信息.

                //使用s_gateway存放信息。因所取的信息全为key信息，所以使用gateway为返回信息对象变量。
                s_gateway->data = key_json_r->all_key->data;
                s_gateway->len = key_json_r->all_key->len;
                break;
            }

            case 112:{             
                if(ngx_http_echo_iad_arg(r, (u_char*)"iad", 3, s_iad)!=NGX_OK){i_state = -101;break;}//无参数iad - 密钥 
                if(s_iad->len != computed_arg->len){ i_state = -102;break;}//参数iad - 密钥 长度不一致 
                if(ngx_strncmp(s_iad->data,computed_arg->data,computed_arg->len) != 0){ i_state = -103;break;}//参数iad 不等于 密钥
                
                ngx_shmap_flush_all(zone);//清空整个字典

                break;
            }

            case 501:{
                if(!(r->method & NGX_HTTP_POST)){i_state = -120;break;}//新增操作必需为post方式          
                if(ngx_http_echo_iad_arg(r, (u_char*)"iad", 3, s_iad)!=NGX_OK){i_state = -101;break;}//无参数iad - 密钥                 
                if(s_iad->len != computed_arg->len){ i_state = -102;break;}//参数iad - 密钥 长度不一致 
                if(ngx_strncmp(s_iad->data,computed_arg->data,computed_arg->len) != 0){ i_state = -103;break;}//参数iad 不等于 密钥
                if(ngx_http_echo_iad_arg(r, (u_char*)"data", 4, s_data)!=NGX_OK){ i_state = -504;break;}//无参数data信息                

                ngx_shmap_get(zone,&ngx_http_iad_domain, s_domain,&vt_str_cache,0,0);//检测key是否存在
                
                if(s_domain->len > 0){
                    ngx_shmap_replace(zone, &ngx_http_iad_domain,s_data,VT_STRING,0,0);//key 对应 value 已存在
                }else{
                    ngx_shmap_add(zone, &ngx_http_iad_domain,s_data,VT_STRING,0,0);//加入缓存数据
                }

                ngx_str_set(s_domain,s_data->data);//装载data信息                 
                s_domain->len = s_data->len;//设置信息长度                

                break;
            }



            case 9999:{
                //无效区，放一些暂时不用的东东
                ngx_http_echo_iad_cache_key(r,s_gateway,s_app_type);
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,":get Go len %d ]]",s_app_type->len);//打印内容
                break;
            }
                    
        }        

        //拼接json数据                
        if(i_state == 0){          		
			if(data_map_value->len > 0){
			str_json_len =  data_map_value->len + s_domain->len + 70;
            str_json_data_p = ngx_palloc(r->pool,str_json_len);  
			(void) ngx_snprintf(str_json_data_p, str_json_len,
                                "{\"state\":%d,\"domain\":\"%V\",\"gateway\":\"%V\",\"data\":%V,\"time\":\"%T\"}",
                                i_state,
                                s_domain,
                                s_gateway,
                                data_map_value,
                                ngx_time()
                                );
			}else{		
			str_json_len =  s_domain->len + s_gateway->len + 66;
            str_json_data_p = ngx_palloc(r->pool,str_json_len);  
            (void) ngx_snprintf(str_json_data_p, str_json_len,
                                "{\"state\":%d,\"domain\":\"%V\",\"gateway\":\"%V\",\"data\":\"\",\"time\":\"%T\"}",
                                i_state,
                                s_domain,
                                s_gateway,
                                ngx_time()
                                );
			}
        }else{       
            str_json_len = 34 ;
            if(i_state > 0 && i_state < 10) str_json_len =  31;
            if(i_state >= 10 && i_state < 100) str_json_len =  32;
            if(i_state >= 100) str_json_len =  33;
            if(i_state < 0 && i_state > -10) str_json_len =  32;
            if(i_state <= -10 && i_state > -100) str_json_len =  33;
            if(i_state <= -100) str_json_len =  34;

            str_json_data_p = ngx_palloc(r->pool,str_json_len);  
            (void) ngx_snprintf(str_json_data_p, str_json_len,
                                "{\"state\":%d,\"time\":\"%T\"}",
                                i_state,                                
                                ngx_time()
                                );
        }        
        
        ngx_str_send_json = ngx_http_echo_iad_str_palloc(r);
        ngx_str_send_json->data = str_json_data_p;
        ngx_str_send_json->len = str_json_len;
        
        buf = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));//开创ngx_buf_t类型内存空间
        if (buf == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        buf->memory = 1;

        buf->start = buf->pos = ngx_str_send_json->data;
        buf->last = buf->end = ngx_str_send_json->data + ngx_str_send_json->len;


        if (cl == NULL) {
            cl = ngx_alloc_chain_link(r->pool);
            if (cl == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            cl->buf  = buf;
            cl->next = NULL;
            ll = &cl->next;

        } else {
            /* append a space first */
            *ll = ngx_alloc_chain_link(r->pool);

            if (*ll == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            space_buf = ngx_calloc_buf(r->pool);

            if (space_buf == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            /* nginx clears buf flags at the end of each request handling,
             * so we have to make a clone here. */
            *space_buf = ngx_http_echo_space_buf;

            (*ll)->buf = space_buf;
            (*ll)->next = NULL;

            ll = &(*ll)->next;

            /* then append the buf only if it's non-empty */
            if (buf) {
                *ll = ngx_alloc_chain_link(r->pool);
                if (*ll == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                (*ll)->buf  = buf;
                (*ll)->next = NULL;

                ll = &(*ll)->next;
            }
        }
    } /* end for */

    if (opts && opts->nelts > 0) {
        opt = opts->elts;
        if (opt[0].len == 1 && opt[0].data[0] == 'n') {
            goto done;
        }
    }

    /* append the newline character */

    if (cl && cl->buf == NULL) {
        cl = cl->next;
    }

    newline_buf = ngx_calloc_buf(r->pool);

    if (newline_buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    *newline_buf = ngx_http_echo_newline_buf;

    if (cl == NULL) {
        cl = ngx_alloc_chain_link(r->pool);

        if (cl == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        cl->buf = newline_buf;
        cl->next = NULL;
        /* ll = &cl->next; */

    } else {
        *ll = ngx_alloc_chain_link(r->pool);

        if (*ll == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        (*ll)->buf  = newline_buf;
        (*ll)->next = NULL;
        /* ll = &(*ll)->next; */
    }

done:

    if (cl == NULL || cl->buf == NULL) {
        return NGX_OK;
    }

    if (in_filter) {
        return ngx_http_echo_next_body_filter(r, cl);
    }

    return ngx_http_echo_send_chain_link(r, ctx, cl);
}


ngx_int_t
ngx_http_echo_exec_iad_flush(ngx_http_request_t *r, ngx_http_echo_ctx_t *ctx)
{
    return ngx_http_send_special(r, NGX_HTTP_FLUSH);
}


ngx_int_t
ngx_http_echo_exec_iad_request_body(ngx_http_request_t *r,
    ngx_http_echo_ctx_t *ctx)
{
    ngx_buf_t       *b;
    ngx_chain_t     *out, *cl, **ll;

    if (r->request_body == NULL || r->request_body->bufs == NULL) {
        return NGX_OK;
    }

    out = NULL;
    ll = &out;

    for (cl = r->request_body->bufs; cl; cl = cl->next) {
        if (ngx_buf_special(cl->buf)) {
            /* we do not want to create zero-size bufs */
            continue;
        }

        *ll = ngx_alloc_chain_link(r->pool);
        if (*ll == NULL) {
            return NGX_ERROR;
        }

        b = ngx_alloc_buf(r->pool);
        if (b == NULL) {
            return NGX_ERROR;
        }

        (*ll)->buf = b;
        (*ll)->next = NULL;

        ngx_memcpy(b, cl->buf, sizeof(ngx_buf_t));
        b->tag = (ngx_buf_tag_t) &ngx_http_echo_exec_iad_request_body;
        b->last_buf = 0;
        b->last_in_chain = 0;

        ll = &(*ll)->next;
    }

    if (out == NULL) {
        return NGX_OK;
    }

    return ngx_http_echo_send_chain_link(r, ctx, out);
}


ngx_int_t
ngx_http_echo_exec_iad_duplicate(ngx_http_request_t *r,
    ngx_http_echo_ctx_t *ctx, ngx_array_t *computed_args)
{
    ngx_str_t                   *computed_arg;
    ngx_str_t                   *computed_arg_elts;
    ssize_t                     i, count;
    ngx_str_t                   *str;
    u_char                      *p;
    ngx_int_t                   rc;

    ngx_buf_t                   *buf;
    ngx_chain_t                 *cl;


    dd_enter();

    computed_arg_elts = computed_args->elts;

    computed_arg = &computed_arg_elts[0];

    count = ngx_http_echo_atosz(computed_arg->data, computed_arg->len);

    if (count == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "invalid size specified: \"%V\"", computed_arg);

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    str = &computed_arg_elts[1];

    if (count == 0 || str->len == 0) {
        rc = ngx_http_echo_send_header_if_needed(r, ctx);
        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }

        return NGX_OK;
    }

    buf = ngx_create_temp_buf(r->pool, count * str->len);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = buf->pos;
    for (i = 0; i < count; i++) {
        p = ngx_copy(p, str->data, str->len);
    }
    buf->last = p;

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cl->next = NULL;
    cl->buf = buf;

    return ngx_http_echo_send_chain_link(r, ctx, cl);
}


static ngx_str_t* ngx_http_echo_iad_str_palloc(ngx_http_request_t *r)
{
    ngx_str_t *value;
    value = ngx_palloc(r->pool,sizeof(ngx_str_t));
    value->len = 0;
    return value;
}


static ngx_str_t* ngx_http_echo_iad_timestamp(ngx_http_request_t *r)
{
    ngx_str_t       *ngx_timestamp;    
    ngx_tm_t        *gmt;
    u_char          *p_time;

    ngx_timestamp = ngx_http_echo_iad_str_palloc(r);
    gmt = ngx_palloc(r->pool,sizeof(ngx_tm_t));
    p_time = ngx_palloc(r->pool,8);

    ngx_gmtime(ngx_time() + 28800 , gmt); // 28800 = 8 * 60 * 60 在中国地区需要 加 8小时
    
    (void) ngx_snprintf(p_time, 8, "%4d%02d%02d",gmt->ngx_tm_year,gmt->ngx_tm_mon,gmt->ngx_tm_mday);

    ngx_timestamp->data = p_time + 2;//偏移二个指针，"20140506" -> "140506" 
    ngx_timestamp->len = 6;

    return ngx_timestamp;
    
}


static ngx_int_t ngx_http_echo_iad_arg(ngx_http_request_t *r, u_char *name, size_t len,ngx_str_t *value)
{   

    if (r->method & NGX_HTTP_GET) {
        return ngx_http_arg(r, name, len, value);
    }

    if (r->method & NGX_HTTP_POST) {
        u_char  *p, *last, *start, *end;
   
        p = r->request_body->bufs->buf->pos;
        last = r->request_body->bufs->buf->last;
        start = r->request_body->bufs->buf->pos;
        end = r->request_body->bufs->buf->last;   
        
                

        for ( /* void */ ; p < last; p++) {

            p = ngx_strlcasestrn(p, last - 1, name, len - 1);

            if (p == NULL) {
                return NGX_DECLINED;
            }

            if ((p == start || *(p - 1) == '&') && *(p + len) == '=') {

                value->data = p + len + 1;

                p = ngx_strlchr(p, last, '&');

                if (p == NULL) {
                    p = end;
                }

                value->len = p - value->data;

                return NGX_OK;
            }
        }
    }
    
    return NGX_DECLINED;
}

static ngx_str_t* ngx_http_echo_iad_cache_key(ngx_http_request_t *r,ngx_str_t *gateway,ngx_str_t *type)
{
    ngx_str_t       *s_key;
    u_char          *s_gt_value;
    size_t          i_size;

    
    
    s_key = ngx_http_echo_iad_str_palloc(r);    
    
    if(gateway->len <= 0 || type->len <=0){ return s_key;}
        
    i_size = gateway->len + type->len + 1;

    s_gt_value = ngx_palloc(r->pool,i_size);    

    (void) ngx_snprintf(s_gt_value, i_size, "%V_%V",gateway,type);

    s_key->data = s_gt_value; 
    s_key->len = i_size;

    return s_key;
    
}

/**
 查询所有键信息-回调方法
*/
void ngx_http_echo_iad_foreach_pt(ngx_shmap_node_t* node, void* extarg)
{
    ngx_http_request_t              *r;//请求变更指针
    ngx_str_t                       *node_data;//遍例回调返回的缓存对象之一
    u_char                          *str_json_data_p;//拼接之前与现遍例变量的字符串
    size_t                          str_json_len;//拼接是需要长度信息

    ngx_str_t                       *all_key_json_data;//之前拼接的甩的key信息
    ngx_http_iad_all_key_json_data_t *key_json_r; //请求和全部key的结构体

    
    key_json_r = extarg;//申明extarg为ngx_http_iad_all_key_json_data_t结构体
    r = key_json_r->r;//获取请求对象信息，
    all_key_json_data = key_json_r->all_key;//获取之前的所有key信息

    node_data = ngx_http_echo_iad_str_palloc(r);//为现在遍例的缓存对象分配内存
    node_data->data = node->data;//赋值信息
    node_data->len = (size_t) node->key_len;//赋值长度   
  
    
    if(all_key_json_data->len == 0){ //若是缓存的第一个对象遍例。
        str_json_len =  node->key_len; 
        str_json_data_p = ngx_palloc(r->pool,str_json_len);
        (void) ngx_snprintf(str_json_data_p, str_json_len,"%V",node_data);//拼接信息
    }else{//若追加信息
        str_json_len =  all_key_json_data->len + node->key_len +1; 
        str_json_data_p = ngx_palloc(r->pool,str_json_len);
        (void) ngx_snprintf(str_json_data_p, str_json_len,"%V|%V",all_key_json_data,node_data);//拼接信息
    }    

    //拼接完成后，放回存放所有key信息的字符串对象
    all_key_json_data->data = str_json_data_p;
    all_key_json_data->len = str_json_len;

    
}
