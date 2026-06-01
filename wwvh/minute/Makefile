WWVH_CLIPS=h_0.raw h_1.raw h_10.raw h_11.raw h_12.raw h_13.raw h_14.raw h_15.raw h_16.raw h_17.raw h_18.raw h_19.raw h_2.raw h_20.raw h_21.raw h_22.raw h_23.raw h_24.raw h_25.raw h_26.raw h_27.raw h_28.raw h_29.raw h_3.raw h_30.raw h_31.raw h_32.raw h_33.raw h_34.raw h_35.raw h_36.raw h_37.raw h_38.raw h_39.raw h_4.raw h_40.raw h_41.raw h_42.raw h_43.raw h_44.raw h_45.raw h_46.raw h_47.raw h_48.raw h_49.raw h_5.raw h_50.raw h_51.raw h_52.raw h_53.raw h_54.raw h_55.raw h_56.raw h_57.raw h_58.raw h_59.raw h_6.raw h_7.raw h_8.raw h_9.raw h_at_the_tone.raw h_hour.raw h_hours.raw h_ident_better.raw h_ident.raw h_minute.raw h_minutes.raw h_utc.raw

.PHONY: all clean

all: $(WWVH_CLIPS)

clean:
	rm -f $(WWVH_CLIPS)


%.raw: %.mp3
	sox $< -t raw --rate 48000 --channels 1 --encoding signed-integer --bits 16 --endian little $@ 
