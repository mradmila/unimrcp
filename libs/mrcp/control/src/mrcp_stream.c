/*
 * Copyright 2008 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mrcp_stream.h"
#include "mrcp_message.h"
#include "mrcp_generic_header.h"
#include "mrcp_resource_factory.h"
#include "apt_log.h"

/** MRCP parser */
struct mrcp_parser_t {
	mrcp_resource_factory_t *resource_factory;
	apt_str_t                resource_name;
	mrcp_stream_result_e     result;
	char                    *pos;
	mrcp_message_t          *message;
	apr_pool_t              *pool;
};

/** MRCP generator */
struct mrcp_generator_t {
	mrcp_resource_factory_t *resource_factory;
	mrcp_stream_result_e     result;
	char                    *pos;
	mrcp_message_t          *message;
	apr_pool_t              *pool;
};


/** Read MRCP message-body */
static mrcp_stream_result_e mrcp_message_body_read(mrcp_message_t *message, apt_text_stream_t *stream)
{
	mrcp_stream_result_e result = MRCP_STREAM_MESSAGE_COMPLETE;
	if(message->body.buf) {
		mrcp_generic_header_t *generic_header = mrcp_generic_header_get(message);
		/* stream length available to read */
		apr_size_t stream_length = stream->text.length - (stream->pos - stream->text.buf);
		/* required/remaining length to read */
		apr_size_t required_length = generic_header->content_length - message->body.length;
		if(required_length > stream_length) {
			required_length = stream_length;
			/* not complete */
			result = MRCP_STREAM_MESSAGE_TRUNCATED;
		}
		memcpy(message->body.buf+message->body.length,stream->pos,required_length);
		message->body.length += required_length;
		stream->pos += required_length;
	}

	return result;
}

/** Parse MRCP message-body */
static mrcp_stream_result_e mrcp_message_body_parse(mrcp_message_t *message, apt_text_stream_t *stream, apr_pool_t *pool)
{
	if(mrcp_generic_header_property_check(message,GENERIC_HEADER_CONTENT_LENGTH) == TRUE) {
		mrcp_generic_header_t *generic_header = mrcp_generic_header_get(message);
		if(generic_header && generic_header->content_length) {
			apt_str_t *body = &message->body;
			body->buf = apr_palloc(pool,generic_header->content_length+1);
			body->length = 0;
			return mrcp_message_body_read(message,stream);
		}
	}
	return MRCP_STREAM_MESSAGE_COMPLETE;
}

/** Write MRCP message-body */
static mrcp_stream_result_e mrcp_message_body_write(mrcp_message_t *message, apt_text_stream_t *stream)
{
	mrcp_stream_result_e result = MRCP_STREAM_MESSAGE_COMPLETE;
	mrcp_generic_header_t *generic_header = mrcp_generic_header_get(message);
	if(generic_header && message->body.length < generic_header->content_length) {
		/* stream length available to write */
		apr_size_t stream_length = stream->text.length - (stream->pos - stream->text.buf);
		/* required/remaining length to write */
		apr_size_t required_length = generic_header->content_length - message->body.length;
		if(required_length > stream_length) {
			required_length = stream_length;
			/* not complete */
			result = MRCP_STREAM_MESSAGE_TRUNCATED;
		}

		memcpy(stream->pos,message->body.buf+message->body.length,required_length);
		message->body.length += required_length;
		stream->pos += required_length;
	}

	return result;
}

/** Generate MRCP message-body */
static mrcp_stream_result_e mrcp_message_body_generate(mrcp_message_t *message, apt_text_stream_t *stream)
{
	if(mrcp_generic_header_property_check(message,GENERIC_HEADER_CONTENT_LENGTH) == TRUE) {
		mrcp_generic_header_t *generic_header = mrcp_generic_header_get(message);
		if(generic_header && generic_header->content_length) {
			apt_str_t *body = &message->body;
			body->length = 0;
			return mrcp_message_body_write(message,stream);
		}
	}
	return MRCP_STREAM_MESSAGE_COMPLETE;
}

/** Create MRCP stream parser */
MRCP_DECLARE(mrcp_parser_t*) mrcp_parser_create(mrcp_resource_factory_t *resource_factory, apr_pool_t *pool)
{
	mrcp_parser_t *parser = apr_palloc(pool,sizeof(mrcp_parser_t));
	parser->resource_factory = resource_factory;
	apt_string_reset(&parser->resource_name);
	parser->result = MRCP_STREAM_MESSAGE_INVALID;
	parser->pos = NULL;
	parser->message = NULL;
	parser->pool = pool;
	return parser;
}

/** Set resource name to be used while parsing (MRCPv1 only) */
MRCP_DECLARE(void) mrcp_parser_resource_name_set(mrcp_parser_t *parser, const apt_str_t *resource_name)
{
	if(resource_name) {
		apt_string_copy(&parser->resource_name,resource_name,parser->pool);
	}
}

static mrcp_stream_result_e mrcp_parser_break(mrcp_parser_t *parser, apt_text_stream_t *stream)
{
	/* failed to parse either start-line or header */
	if(apt_text_is_eos(stream) == TRUE) {
		/* end of stream reached, rewind/restore stream */
		stream->pos = parser->pos;
		parser->result = MRCP_STREAM_MESSAGE_TRUNCATED;
		parser->message = NULL;
	}
	else {
		/* error case */
		parser->result = MRCP_STREAM_MESSAGE_INVALID;
	}
	return parser->result;
}

/** Parse MRCP stream */
MRCP_DECLARE(mrcp_stream_result_e) mrcp_parser_run(mrcp_parser_t *parser, apt_text_stream_t *stream)
{
	mrcp_message_t *message = parser->message;
	if(message && parser->result == MRCP_STREAM_MESSAGE_TRUNCATED) {
		/* process continuation data */
		parser->result = mrcp_message_body_read(message,stream);
		return parser->result;
	}
	
	/* create new MRCP message */
	message = mrcp_message_create(parser->pool);
	message->channel_id.resource_name = parser->resource_name;
	parser->message = message;
	/* store current position to be able to rewind/restore stream if needed */
	parser->pos = stream->pos;
	/* parse start-line */
	if(mrcp_start_line_parse(&message->start_line,stream,message->pool) == FALSE) {
		return mrcp_parser_break(parser,stream);
	}

	if(message->start_line.version == MRCP_VERSION_2) {
		mrcp_channel_id_parse(&message->channel_id,stream,message->pool);
	}

	if(mrcp_message_resourcify_by_name(parser->resource_factory,message) == FALSE) {
		return MRCP_STREAM_MESSAGE_INVALID;
	}

	/* parse header */
	if(mrcp_message_header_parse(&message->header,stream,message->pool) == FALSE) {
		return mrcp_parser_break(parser,stream);
	}

	/* parse body */
	parser->result = mrcp_message_body_parse(message,stream,message->pool);
	return parser->result;
}

/** Get parsed MRCP message */
MRCP_DECLARE(mrcp_message_t*) mrcp_parser_message_get(const mrcp_parser_t *parser)
{
	return parser->message;
}


/** Create MRCP stream generator */
MRCP_DECLARE(mrcp_generator_t*) mrcp_generator_create(mrcp_resource_factory_t *resource_factory, apr_pool_t *pool)
{
	mrcp_generator_t *generator = apr_palloc(pool,sizeof(mrcp_generator_t));
	generator->resource_factory = resource_factory;
	generator->result = MRCP_STREAM_MESSAGE_INVALID;
	generator->pos = NULL;
	generator->message = NULL;
	generator->pool = pool;
	return generator;
}

/** Set MRCP message to generate */
MRCP_DECLARE(apt_bool_t) mrcp_generator_message_set(mrcp_generator_t *generator, mrcp_message_t *message)
{
	if(!message) {
		return FALSE;
	}
	generator->message = message;
	return TRUE;
}

static mrcp_stream_result_e mrcp_generator_break(mrcp_generator_t *generator, apt_text_stream_t *stream)
{
	/* failed to generate either start-line or header */
	if(apt_text_is_eos(stream) == TRUE) {
		/* end of stream reached, rewind/restore stream */
		stream->pos = generator->pos;
		generator->result = MRCP_STREAM_MESSAGE_TRUNCATED;
	}
	else {
		/* error case */
		generator->result = MRCP_STREAM_MESSAGE_INVALID;
	}
	return generator->result;
}

/** Generate MRCP stream */
MRCP_DECLARE(mrcp_stream_result_e) mrcp_generator_run(mrcp_generator_t *generator, apt_text_stream_t *stream)
{
	mrcp_message_t *message = generator->message;
	if(!message) {
		return MRCP_STREAM_MESSAGE_INVALID;
	}

	if(message && generator->result == MRCP_STREAM_MESSAGE_TRUNCATED) {
		/* process continuation data */
		generator->result = mrcp_message_body_write(message,stream);
		return generator->result;
	}

	/* initialize resource specific data */
	if(mrcp_message_resourcify_by_id(generator->resource_factory,message) == FALSE) {
		return MRCP_STREAM_MESSAGE_INVALID;
	}

	/* validate message */
	if(mrcp_message_validate(message) == FALSE) {
		return MRCP_STREAM_MESSAGE_INVALID;
	}
	
	/* generate start-line */
	if(mrcp_start_line_generate(&message->start_line,stream) == FALSE) {
		return mrcp_generator_break(generator,stream);
	}

	if(message->start_line.version == MRCP_VERSION_2) {
		mrcp_channel_id_generate(&message->channel_id,stream);
	}

	/* generate header */
	if(mrcp_message_header_generate(&message->header,stream) == FALSE) {
		return mrcp_generator_break(generator,stream);
	}

	/* finalize start-line generation */
	mrcp_start_line_finalize(&message->start_line,message->body.length,stream);

	/* generate body */
	generator->result = mrcp_message_body_generate(message,stream);
	return generator->result;
}


/** Walk through MRCP stream and invoke message handler for each parsed message */
MRCP_DECLARE(apt_bool_t) mrcp_stream_walk(mrcp_parser_t *parser, apt_text_stream_t *stream, mrcp_message_handler_f handler, void *obj)
{
	mrcp_stream_result_e result;
	do {
		result = mrcp_parser_run(parser,stream);
		if(result == MRCP_STREAM_MESSAGE_COMPLETE) {
			/* message is partially parsed, to be continued */
			apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Parsed MRCP Message [%lu]", stream->pos - stream->text.buf);
		}
		else if(result == MRCP_STREAM_MESSAGE_TRUNCATED) {
			/* message is partially parsed, to be continued */
			apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Truncated MRCP Message [%lu]", stream->pos - stream->text.buf);
		}
		else if(result == MRCP_STREAM_MESSAGE_INVALID){
			/* error case */
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Failed to Parse MRCP Message");
		}

		/* invoke message handler */
		if(handler(obj,parser->message,result) == FALSE) {
			return FALSE;
		}
	}
	while(apt_text_is_eos(stream) == FALSE  &&  result != MRCP_STREAM_MESSAGE_TRUNCATED);

	/* prepare stream for further processing */
	if(result == MRCP_STREAM_MESSAGE_TRUNCATED) {
		if(apt_text_stream_scroll(stream) == TRUE) {
			apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Scroll MRCP Stream [%d]",stream->pos - stream->text.buf);
		}
		else {
			stream->pos = stream->text.buf;
		}
	}
	else {
		stream->pos = stream->text.buf;
	}
	return TRUE;
}