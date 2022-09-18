#pragma once

#include <iostream>
#include <unordered_map>
#include <list>
#include "memory/StackAllocator.hpp"
#include <cassert>

class Hermes{
	public:
		using EventID = uint16_t;

		// if the functions/methods return true, that mean that the event ha sbeen handled and should no longer be send to other callbacks
		using EventFn = bool(*)(void*); // event function (the data pointer)
		using EventMt = bool(*)(void*, void*); // event method (the instance pointer and the data pointer)

		union Callback{
			EventFn function;
			EventMt method;
		};

		class DataLayout{
			friend class Hermes;
			public:
				template<typename... Args>
				static DataLayout construct(){
					DataLayout layout;
					layout.add<Args...>();
					return layout;
				}

			private:
				template<typename T, typename A, typename... Args>
				void add(){
					add<T>();
					add<A, Args...>();
				}
				
				template<typename T>
				void add(){
					size += sizeof(T);
				}

				uint16_t size = 0;
		};

		Hermes() = default;

		static void initialize(uint16_t eventTypeCount, uint32_t bufferSize){
			Hermes &instance = getInstance();
			instance.maxAvailableEventTypeCount = eventTypeCount;
			instance.events = new EventType[eventTypeCount];

			for (int i=0; i<eventTypeCount; i++){
				instance.events[i].callbacks = new std::list<EventCallback>();
				
			}

			instance.dataBuffer = new StackAllocator(bufferSize);
		}

		static void shutdown(){
			Hermes &instance = getInstance();
			for (int i=0; i<instance.maxAvailableEventTypeCount; i++){
				delete instance.events[i].callbacks;
			}

			delete[] instance.events;
			delete instance.dataBuffer;

			instance.events = nullptr;
			instance.dataBuffer = nullptr;
		}

		static EventID registerEvent(const char* name, uint16_t dataSize = 0){
			Hermes& instance = getInstance();
			assert(instance.registeredEventCount <= instance.maxAvailableEventTypeCount && "event type overflow");

			EventID &registeredEventCount = instance.registeredEventCount;

			// check if the name is already used
			{
				auto iterator = instance.eventMap.find(name);
				if (iterator != instance.eventMap.end()){
					return iterator->second;
				}
			}

			// create the evnt type from the given informations
			EventType &event = instance.events[registeredEventCount];
			event.id = registeredEventCount;
			event.dataSize = dataSize;

			EventID id = registeredEventCount;
			registeredEventCount++;

			instance.eventMap[name] = id;
			return id;
		}

		static EventID registerEvent(const char* name, DataLayout data){
			return registerEvent(name, data.size);
		}

		static EventID getEventIndex(const char* name){
			Hermes& instance = getInstance();
			auto iterator = instance.eventMap.find(name);
			assert(iterator != instance.eventMap.end() && "unknown event name");
			return iterator->second;
		}

		template<typename... Args>
		static void triggerEvent(const char* name, Args... args){
			triggerEvent(getEventIndex(name), args...);
		}

		template<typename T, typename... Args>
		static void triggerEvent(EventID id, T t, Args... args){
			Hermes& instance = getInstance();
			void* data = _convert(instance.events[id].dataSize, t, args...);
			_triggerEvent(id, data);
		}

		static void triggerEvent(EventID id){
			Hermes& instance = getInstance();
			_triggerEvent(id, nullptr);
		}

		template<typename... Args>
		static void convert(void* ptr, Args&... args){
			size_t offset = 0;
			convertFromVoid(ptr, offset, args...);
		}

		static void subscribe(const char *name, EventFn callback){
			subscribe(getInstance().getEventIndex(name), callback);
		}

		static void subscribe(const char *name, void* subscribedInstance, EventMt callback){
			subscribe(getInstance().getEventIndex(name), subscribedInstance, callback);
		}
	
		static void subscribe(EventID id, EventFn callback){
			Hermes &instance = getInstance();
			assert(instance.registeredEventCount >= id && "event type overflow");

			EventType& event = instance.events[id];
			EventCallback cb;
			cb.callback.function = callback;
			cb.type = cb.Function;
			event.callbacks->push_back(cb);
		}

		static void subscribe(EventID id, void* subscribedInstance, EventMt callback){
			Hermes &instance = getInstance();
			assert(instance.registeredEventCount >= id && "event type overflow");

			EventType& event = instance.events[id];
			EventCallback cb;
			cb.callback.method = callback;
			cb.type = cb.Method;
			event.callbacks->push_back(cb);
		}

		// this function will call all callbacks of the triggered events and then reset the data buffer
		static void update(){
			Hermes &instance = getInstance();
			for (auto &c : instance.calls){
				EventType& event = instance.events[c.id];
				
				for (auto &callback : *event.callbacks){
					if (callCallback(callback, c.data)) break;
				}
			}

			instance.calls.clear();
			instance.dataBuffer->clear();
		}

		static EventID getRegisteredEventCount(){
			return getInstance().registeredEventCount;
		}

		static EventID getMaxEventTypeCount(){
			return getInstance().maxAvailableEventTypeCount;
		}

		static size_t getMaxDataBufferSize(){
			return getInstance().dataBuffer->getMaxSize();
		}

		static size_t getCurrentlyUsedDataBufferSize(){
			return getInstance().dataBuffer->getCurrentUsedSize();
		}
	
	private:
		static Hermes& getInstance(){
			extern Hermes instance;
			return instance;
		}

		struct EventCallback{
			enum CallbackType{
				Function,
				Method,
			};

			Callback callback;
			CallbackType type;
			void* subscribedInstance = nullptr;
		};

		struct EventType{
			EventID id = 0;
			uint16_t dataSize = 0;
			std::list<EventCallback>* callbacks = nullptr;
		};

		struct EventCall{
			EventID id;
			void* data = nullptr;
		};

		template<typename T, typename... Args>
		static void convertFromVoid(void* ptr, size_t &offset, T &t, Args&... args){
			void* data = static_cast<void*>(static_cast<char*>(ptr) + offset);
			offset += sizeof(T);
			t = *static_cast<T*>(data);
			convertFromVoid(ptr, offset, args...);
		}

		template<typename T>
		static void convertFromVoid(void* ptr, size_t &offset, T &t){
			void* data = static_cast<void*>(static_cast<char*>(ptr) + offset);
			offset += sizeof(T);
			t = *static_cast<T*>(data);
		}


		template<typename... Args>
		static void* _convert(size_t size, Args... args){
			Hermes& instance = getInstance();
			void* ptr = instance.dataBuffer->push(size);
			size_t offset = 0;
			return __convert(ptr, offset, size, args...);
		}

		template<typename T, typename... Args>
		static void* __convert(void* data, size_t &offset, size_t &maxSize, T t, Args... args){
			void* ptr = static_cast<void*>(static_cast<char*>(data) + offset);
			assert(offset + sizeof(T) <= maxSize && "data overflow");
			memcpy(ptr, &t, sizeof(T));
			offset+=sizeof(T);
			return __convert(data, offset, maxSize, args...);
		}

		template<typename T>
		static void* __convert(void* data, size_t &offset, size_t &maxSize, T t){
			void* ptr = static_cast<void*>(static_cast<char*>(data) + offset);
			assert(offset + sizeof(T) <= maxSize && "data overflow");
			memcpy(ptr, &t, sizeof(T));
			offset+=sizeof(T);
			return data;
		}
		
		static void _triggerEvent(EventID eventID, void* data){
			Hermes& instance = getInstance();
			assert(eventID < instance.registeredEventCount && "event type overflow");
			EventCall call;
			call.id = eventID;
			call.data = data;

			instance.calls.push_back(call);
		}

		static bool callCallback(EventCallback &callback, void* data){
			switch (callback.type){
			case EventCallback::Function: return callback.callback.function(data);
			case EventCallback::Method: return callback.callback.method(callback.subscribedInstance, data);
		}
		return false;
		}

		EventType* events;
		StackAllocator *dataBuffer;
		std::list<EventCall> calls;
		std::unordered_map<std::string, EventID> eventMap;
		EventID registeredEventCount = 0;
		EventID maxAvailableEventTypeCount = 0;
};